/**
 * @file NetworkServer.cpp
 * @brief Implémente le cycle de vie commun du récepteur réseau STGS.
 *
 * Les boucles UDP et TCP sont volontairement placées dans des unités de compilation séparées afin
 * que chaque transport conserve ses invariants propres sans transformer ce fichier en fourre-tout.
 */
#include "stgs/NetworkServer.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace stgs
{
    namespace
    {

        void closeFd(int &fd) noexcept
        {
            if (fd >= 0)
            {
                ::close(fd);
                fd = -1;
            }
        }

        class ScopedFd
        {
        public:
            explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
            ~ScopedFd() { closeFd(fd_); }
            ScopedFd(const ScopedFd &) = delete;
            ScopedFd &operator=(const ScopedFd &) = delete;

            [[nodiscard]] int get() const noexcept { return fd_; }
            [[nodiscard]] int release() noexcept
            {
                const int fd = fd_;
                fd_ = -1;
                return fd;
            }

        private:
            int fd_ = -1;
        };

        sockaddr_in makeAddress(const std::string &bindAddress, std::uint16_t port)
        {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1)
            {
                throw std::runtime_error("invalid IPv4 bind address: " + bindAddress);
            }
            return addr;
        }

    } // namespace

    NetworkServer::NetworkServer(NetworkConfig config, Logger &logger)
        : config_(std::move(config)), logger_(logger)
    {
        if (config_.port == 0U)
        {
            throw std::invalid_argument("network port must be in the 1..65535 range");
        }
        if (config_.pollTimeoutMs <= 0)
        {
            throw std::invalid_argument("network poll timeout must be greater than zero");
        }
    }

    NetworkServer::~NetworkServer()
    {
        stop();
        closeServerSocket();
    }

    void NetworkServer::run(FrameCallback callback, const std::atomic_bool &running)
    {
        stopRequested_.store(false);
        try
        {
            if (config_.transport == Transport::Udp)
            {
                runUdp(callback, running);
            }
            else
            {
                runTcp(callback, running);
            }
        }
        catch (...)
        {
            closeServerSocket();
            throw;
        }
    }

    void NetworkServer::stop() noexcept
    {
        // Arrêt coopératif : évite de fermer un descripteur pendant que `poll()` l'utilise ailleurs.
        stopRequested_.store(true);
    }

    int NetworkServer::createBoundSocket(int socketType) const
    {
        ScopedFd socketFd(::socket(AF_INET, socketType, 0));
        if (socketFd.get() < 0)
        {
            throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
        }

        int yes = 1;
        if (::setsockopt(socketFd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
        {
            throw std::runtime_error("setsockopt(SO_REUSEADDR) failed: " + std::string(std::strerror(errno)));
        }

        const auto addr = makeAddress(config_.bindAddress, config_.port);
        if (::bind(socketFd.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            throw std::runtime_error("bind() failed on " + config_.bindAddress + ":" +
                                     std::to_string(config_.port) + ": " + std::strerror(errno));
        }
        return socketFd.release();
    }

    void NetworkServer::closeServerSocket() noexcept
    {
        closeFd(serverFd_);
    }

    std::string transportToString(Transport transport)
    {
        return transport == Transport::Udp ? "udp" : "tcp";
    }

} // namespace stgs
