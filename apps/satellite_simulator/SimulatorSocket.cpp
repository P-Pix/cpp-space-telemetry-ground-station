/**
 * @file SimulatorSocket.cpp
 * @brief Implémente les opérations socket du simulateur STGS.
 */
#include "SimulatorSocket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace stgs::app::simulator
{
    namespace
    {
        std::string normalizeIpv4Host(const std::string &host)
        {
            return host == "localhost" ? "127.0.0.1" : host;
        }
    } // namespace

    SocketFd::SocketFd(int fd) noexcept : fd_(fd) {}

    SocketFd::~SocketFd()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }

    SocketFd::SocketFd(SocketFd &&other) noexcept : fd_(other.fd_)
    {
        other.fd_ = -1;
    }

    SocketFd &SocketFd::operator=(SocketFd &&other) noexcept
    {
        if (this != &other)
        {
            if (fd_ >= 0)
            {
                ::close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int SocketFd::get() const noexcept
    {
        return fd_;
    }

    SocketFd createSocket(Transport transport)
    {
        const int type = transport == Transport::Udp ? SOCK_DGRAM : SOCK_STREAM;
        const int fd = ::socket(AF_INET, type, 0);
        if (fd < 0)
        {
            throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
        }
        return SocketFd(fd);
    }

    sockaddr_in destination(const Options &options)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(options.port);
        const auto host = normalizeIpv4Host(options.host);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            throw std::runtime_error("invalid IPv4 destination host: " + options.host);
        }
        return addr;
    }

    void sendAll(int fd, const ByteVector &bytes)
    {
        std::size_t sent = 0U;
        while (sent < bytes.size())
        {
            const ssize_t count = ::send(fd,
                                         bytes.data() + static_cast<std::ptrdiff_t>(sent),
                                         bytes.size() - sent,
                                         MSG_NOSIGNAL);
            if (count < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                throw std::runtime_error("send() failed: " + std::string(std::strerror(errno)));
            }
            if (count == 0)
            {
                throw std::runtime_error("send() returned zero before the frame was fully transmitted");
            }
            sent += static_cast<std::size_t>(count);
        }
    }

} // namespace stgs::app::simulator
