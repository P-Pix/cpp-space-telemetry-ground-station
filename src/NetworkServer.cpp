#include "stgs/NetworkServer.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace stgs {
namespace {

void closeFd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::runtime_error("fcntl(F_GETFL) failed: " + std::string(std::strerror(errno)));
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("fcntl(F_SETFL) failed: " + std::string(std::strerror(errno)));
    }
}

sockaddr_in makeAddress(const std::string& bindAddress, std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid IPv4 bind address: " + bindAddress);
    }
    return addr;
}

struct TcpClient {
    int fd = -1;
    StreamFrameExtractor extractor;
};

} // namespace

NetworkServer::NetworkServer(NetworkConfig config, Logger& logger)
    : config_(std::move(config)), logger_(logger) {}

NetworkServer::~NetworkServer() {
    stop();
}

void NetworkServer::run(FrameCallback callback, const std::atomic_bool& running) {
    stopRequested_.store(false);
    if (config_.transport == Transport::Udp) {
        runUdp(callback, running);
    } else {
        runTcp(callback, running);
    }
}

void NetworkServer::stop() {
    stopRequested_.store(true);
    closeServerSocket();
}

int NetworkServer::createBoundSocket(int socketType) const {
    int fd = ::socket(AF_INET, socketType, 0);
    if (fd < 0) {
        throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
    }

    int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        ::close(fd);
        throw std::runtime_error("setsockopt(SO_REUSEADDR) failed: " + std::string(std::strerror(errno)));
    }

    const auto addr = makeAddress(config_.bindAddress, config_.port);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("bind() failed on " + config_.bindAddress + ":" +
                                 std::to_string(config_.port) + ": " + std::strerror(errno));
    }

    return fd;
}

void NetworkServer::runUdp(FrameCallback& callback, const std::atomic_bool& running) {
    serverFd_ = createBoundSocket(SOCK_DGRAM);
    logger_.info("UDP receiver listening on " + config_.bindAddress + ":" + std::to_string(config_.port));

    ByteVector buffer(MaxFrameSize);
    while (running.load() && !stopRequested_.load()) {
        pollfd pfd{serverFd_, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, config_.pollTimeoutMs);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("poll() failed: " + std::string(std::strerror(errno)));
        }
        if (rc == 0 || (pfd.revents & POLLIN) == 0) {
            continue;
        }

        sockaddr_in src{};
        socklen_t srcLen = sizeof(src);
        const ssize_t n = ::recvfrom(serverFd_, buffer.data(), buffer.size(), 0,
                                     reinterpret_cast<sockaddr*>(&src), &srcLen);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            logger_.warning("recvfrom() failed: " + std::string(std::strerror(errno)));
            continue;
        }

        ByteVector frame(buffer.begin(), buffer.begin() + n);
        callback(std::move(frame));
    }

    closeServerSocket();
    logger_.info("UDP receiver stopped");
}

void NetworkServer::runTcp(FrameCallback& callback, const std::atomic_bool& running) {
    serverFd_ = createBoundSocket(SOCK_STREAM);
    setNonBlocking(serverFd_);
    if (::listen(serverFd_, SOMAXCONN) < 0) {
        closeServerSocket();
        throw std::runtime_error("listen() failed: " + std::string(std::strerror(errno)));
    }

    logger_.info("TCP receiver listening on " + config_.bindAddress + ":" + std::to_string(config_.port));
    std::vector<TcpClient> clients;
    ByteVector readBuffer(8192);

    while (running.load() && !stopRequested_.load()) {
        std::vector<pollfd> pfds;
        pfds.reserve(clients.size() + 1U);
        pfds.push_back(pollfd{serverFd_, POLLIN, 0});
        for (const auto& client : clients) {
            pfds.push_back(pollfd{client.fd, POLLIN | POLLHUP | POLLERR, 0});
        }

        const int rc = ::poll(pfds.data(), pfds.size(), config_.pollTimeoutMs);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("poll() failed: " + std::string(std::strerror(errno)));
        }
        if (rc == 0) {
            continue;
        }

        if ((pfds[0].revents & POLLIN) != 0) {
            while (true) {
                sockaddr_in peer{};
                socklen_t peerLen = sizeof(peer);
                int clientFd = ::accept(serverFd_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
                if (clientFd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        break;
                    }
                    logger_.warning("accept() failed: " + std::string(std::strerror(errno)));
                    break;
                }
                setNonBlocking(clientFd);
                clients.push_back(TcpClient{clientFd, StreamFrameExtractor{}});
                logger_.info("TCP client connected");
            }
        }

        for (std::size_t i = 0; i < clients.size();) {
            auto& client = clients[i];
            const auto revents = pfds[i + 1U].revents;
            bool removeClient = false;

            if ((revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                removeClient = true;
            } else if ((revents & POLLIN) != 0) {
                while (true) {
                    const ssize_t n = ::recv(client.fd, readBuffer.data(), readBuffer.size(), 0);
                    if (n > 0) {
                        const auto frames = client.extractor.feed(std::span<const std::uint8_t>(readBuffer.data(), static_cast<std::size_t>(n)));
                        for (auto frame : frames) {
                            callback(std::move(frame));
                        }
                    } else if (n == 0) {
                        removeClient = true;
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                            break;
                        }
                        logger_.warning("recv() failed: " + std::string(std::strerror(errno)));
                        removeClient = true;
                        break;
                    }
                }
            }

            if (removeClient) {
                logger_.info("TCP client disconnected");
                closeFd(client.fd);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    for (auto& client : clients) {
        closeFd(client.fd);
    }
    closeServerSocket();
    logger_.info("TCP receiver stopped");
}

void NetworkServer::closeServerSocket() noexcept {
    closeFd(serverFd_);
}

std::string transportToString(Transport transport) {
    return transport == Transport::Udp ? "udp" : "tcp";
}

} // namespace stgs
