/**
 * @file NetworkServerTcp.cpp
 * @brief Implémente la réception TCP multi-clients et le framing STGS par connexion.
 */
#include "stgs/NetworkServer.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace stgs
{
    namespace
    {

        // 8192 octets permettent de lire presque deux trames STGS maximales (~4 KiB chacune) par syscall.
        // StreamFrameExtractor conserve les fragments, donc cette taille n'est pas une limite protocolaire.
        inline constexpr std::size_t TcpReadBufferSize = 8192U;

        void closeFd(int &fd) noexcept
        {
            if (fd >= 0)
            {
                ::close(fd);
                fd = -1;
            }
        }

        void setNonBlocking(int fd)
        {
            const int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags < 0)
            {
                throw std::runtime_error("fcntl(F_GETFL) failed: " + std::string(std::strerror(errno)));
            }
            if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            {
                throw std::runtime_error("fcntl(F_SETFL) failed: " + std::string(std::strerror(errno)));
            }
        }

        class ScopedAcceptedFd
        {
        public:
            explicit ScopedAcceptedFd(int fd) noexcept : fd_(fd) {}
            ~ScopedAcceptedFd() { closeFd(fd_); }
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

        /**
         * @brief Associe une connexion TCP à son extracteur de trames et possède son descripteur.
         *
         * Le type est move-only afin que la réallocation du `std::vector` ne duplique jamais un fd. Sa
         * destruction ferme la connexion même si le callback applicatif ou le framing lève une exception.
         */
        struct TcpClient
        {
            explicit TcpClient(int clientFd) noexcept : fd(clientFd) {}
            ~TcpClient() { closeFd(fd); }

            TcpClient(const TcpClient &) = delete;
            TcpClient &operator=(const TcpClient &) = delete;

            TcpClient(TcpClient &&other) noexcept
                : fd(other.fd), extractor(std::move(other.extractor))
            {
                other.fd = -1;
            }

            TcpClient &operator=(TcpClient &&other) noexcept
            {
                if (this != &other)
                {
                    closeFd(fd);
                    fd = other.fd;
                    extractor = std::move(other.extractor);
                    other.fd = -1;
                }
                return *this;
            }

            int fd = -1;
            StreamFrameExtractor extractor;
        };

    } // namespace

    /**
     * @brief Gère plusieurs clients TCP sans désynchroniser `pollfd` et la liste des connexions.
     *
     * Algorithme :
     * 1. figer `polledClientCount` et construire le snapshot `pollfd` correspondant ;
     * 2. exécuter `poll()` ;
     * 3. traiter uniquement les clients de ce snapshot, en ordre inverse pour permettre `erase()` ;
     * 4. drainer `POLLIN` même si `POLLHUP` accompagne les derniers octets ;
     * 5. accepter seulement ensuite les nouveaux clients, visibles à l'itération suivante.
     *
     * Cette séparation interdit qu'une connexion acceptée après la construction de `pfds` indexe un
     * élément inexistant, classe d'UB qui avait été détectée par AddressSanitizer dans une version
     * antérieure du projet.
     */
    void NetworkServer::runTcp(FrameCallback &callback, const std::atomic_bool &running)
    {
        serverFd_ = createBoundSocket(SOCK_STREAM);
        setNonBlocking(serverFd_);
        if (::listen(serverFd_, SOMAXCONN) < 0)
        {
            closeServerSocket();
            throw std::runtime_error("listen() failed: " + std::string(std::strerror(errno)));
        }

        logger_.info("TCP receiver listening on " + config_.bindAddress + ":" + std::to_string(config_.port));
        std::vector<TcpClient> clients;
        ByteVector readBuffer(TcpReadBufferSize);

        while (running.load() && !stopRequested_.load())
        {
            const std::size_t polledClientCount = clients.size();
            std::vector<pollfd> pfds;
            pfds.reserve(polledClientCount + 1U);
            pfds.push_back(pollfd{serverFd_, POLLIN, 0});
            for (std::size_t i = 0U; i < polledClientCount; ++i)
            {
                pfds.push_back(pollfd{clients[i].fd, POLLIN | POLLHUP | POLLERR, 0});
            }

            const int rc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), config_.pollTimeoutMs);
            if (rc < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                throw std::runtime_error("poll() failed: " + std::string(std::strerror(errno)));
            }
            if (rc == 0)
            {
                continue;
            }

            for (std::size_t snapshotIndex = polledClientCount; snapshotIndex > 0U; --snapshotIndex)
            {
                const std::size_t clientIndex = snapshotIndex - 1U;
                const short revents = pfds[snapshotIndex].revents;
                if (revents == 0)
                {
                    continue;
                }

                TcpClient &client = clients[clientIndex];
                bool removeClient = (revents & (POLLERR | POLLNVAL)) != 0;
                if ((revents & (POLLIN | POLLHUP)) != 0)
                {
                    while (true)
                    {
                        const ssize_t count = ::recv(client.fd, readBuffer.data(), readBuffer.size(), 0);
                        if (count > 0)
                        {
                            const auto bytes = std::span<const std::uint8_t>(
                                readBuffer.data(), static_cast<std::size_t>(count));
                            auto frames = client.extractor.feed(bytes);
                            for (auto &frame : frames)
                            {
                                callback(std::move(frame));
                            }
                            continue;
                        }
                        if (count == 0)
                        {
                            removeClient = true;
                            break;
                        }
                        if (errno == EINTR)
                        {
                            continue;
                        }
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            break;
                        }
                        logger_.warning("recv() failed: " + std::string(std::strerror(errno)));
                        removeClient = true;
                        break;
                    }
                }

                if ((revents & POLLHUP) != 0)
                {
                    removeClient = true;
                }
                if (removeClient)
                {
                    logger_.info("TCP client disconnected");
                    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(clientIndex));
                }
            }

            if ((pfds.front().revents & POLLIN) != 0)
            {
                while (true)
                {
                    sockaddr_in peer{};
                    socklen_t peerLength = sizeof(peer);
                    ScopedAcceptedFd accepted(::accept(serverFd_, reinterpret_cast<sockaddr *>(&peer), &peerLength));
                    if (accepted.get() < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            break;
                        }
                        if (errno == EINTR)
                        {
                            continue;
                        }
                        logger_.warning("accept() failed: " + std::string(std::strerror(errno)));
                        break;
                    }

                    setNonBlocking(accepted.get());
                    clients.emplace_back(accepted.release());
                    logger_.info("TCP client connected");
                }
            }
        }

        clients.clear();
        closeServerSocket();
        logger_.info("TCP receiver stopped");
    }

} // namespace stgs
