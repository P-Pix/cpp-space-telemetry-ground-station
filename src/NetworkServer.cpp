/**
 * @file NetworkServer.cpp
 * @brief Implémente la réception robuste de télémétrie sur UDP et TCP.
 *
 * UDP traite un datagramme comme un candidat indivisible. TCP maintient un extracteur par client,
 * car un seul `recv()` peut contenir une fraction de trame ou plusieurs trames concaténées.
 */

#include "stgs/NetworkServer.hpp"

#include <arpa/inet.h>
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

    /**
     * 8192 octets permettent de lire presque deux trames STGS maximales (~4 KiB chacune) par
     * syscall sans créer un buffer disproportionné. Ce choix n'est pas une limite protocolaire :
     * StreamFrameExtractor conserve les fragments entre plusieurs lectures.
     */
    inline constexpr std::size_t TcpReadBufferSize = 8192U;

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

    /**
     * @brief Associe une connexion TCP à son extracteur de trames et possède son descripteur.
     *
     * La possession RAII est importante sur les chemins d'exception : si le callback applicatif ou
     * le framing lève, la destruction du vector `clients` ferme quand même toutes les connexions.
     * Le type est move-only afin qu'un `std::vector` puisse le réallouer sans dupliquer un fd.
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
        // Le thread qui possède la boucle ferme lui-même la socket, y compris si le callback lève.
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

/**
 * @brief Reçoit des datagrammes UDP complets sans accepter silencieusement une troncature.
 *
 * Le buffer fait exactement MaxFrameSize et `MSG_TRUNC` demande au noyau de retourner la taille
 * réelle du datagramme. Une taille supérieure au buffer est donc identifiable et rejetée, au lieu
 * de présenter au codec un préfixe tronqué qui masquerait la cause réseau. `poll()` garde l'arrêt
 * réactif sans placer la socket UDP en boucle active.
 */
void NetworkServer::runUdp(FrameCallback &callback, const std::atomic_bool &running)
{
    serverFd_ = createBoundSocket(SOCK_DGRAM);
    logger_.info("UDP receiver listening on " + config_.bindAddress + ":" + std::to_string(config_.port));

    ByteVector buffer(MaxFrameSize);
    while (running.load() && !stopRequested_.load())
    {
        pollfd pfd{serverFd_, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, config_.pollTimeoutMs);
        if (rc < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            throw std::runtime_error("poll() failed: " + std::string(std::strerror(errno)));
        }
        if (rc == 0 || (pfd.revents & POLLIN) == 0)
        {
            continue;
        }

        sockaddr_in source{};
        socklen_t sourceLength = sizeof(source);
        const ssize_t received = ::recvfrom(serverFd_,
                                            buffer.data(),
                                            buffer.size(),
                                            MSG_TRUNC,
                                            reinterpret_cast<sockaddr *>(&source),
                                            &sourceLength);
        if (received < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }
            logger_.warning("recvfrom() failed: " + std::string(std::strerror(errno)));
            continue;
        }

        const auto receivedSize = static_cast<std::size_t>(received);
        if (receivedSize > buffer.size())
        {
            logger_.warning("UDP datagram larger than MaxFrameSize was dropped without truncation processing");
            continue;
        }

        callback(ByteVector(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(receivedSize)));
    }

    closeServerSocket();
    logger_.info("UDP receiver stopped");
}

/**
 * @brief Gère plusieurs clients TCP non bloquants sans désynchroniser `pollfd` et clients.
 *
 * Algorithme :
 * 1. construire un snapshot `pollfd` à partir des clients existants ;
 * 2. appeler `poll()` ;
 * 3. traiter uniquement ces clients snapshotés, en ordre inverse pour autoriser `erase()` ;
 * 4. drainer `POLLIN` même lorsqu'un `POLLHUP` accompagne la dernière donnée ;
 * 5. accepter ensuite les nouvelles connexions, qui ne seront pollées qu'à l'itération suivante.
 *
 * Cette séparation est essentielle : ajouter un client avant de parcourir le tableau `pollfd`
 * construit plus tôt provoquerait un accès hors limites. C'était précisément le type d'UB que
 * l'AddressSanitizer doit pouvoir détecter sur ce composant.
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

        // Parcours inverse : supprimer clients[i] ne décale jamais les indices restant à traiter.
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

            // POLLHUP peut être livré avec POLLIN : on draine donc toujours les octets disponibles
            // avant de considérer la connexion fermée.
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
                // erase() détruit le TcpClient et ferme son descripteur via RAII.
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(clientIndex));
            }
        }

        // Les nouvelles connexions sont acceptées après le traitement du snapshot. Elles ne peuvent
        // donc jamais indexer le tableau `pfds` construit avant leur arrivée.
        if ((pfds.front().revents & POLLIN) != 0)
        {
            while (true)
            {
                sockaddr_in peer{};
                socklen_t peerLength = sizeof(peer);
                ScopedFd accepted(::accept(serverFd_, reinterpret_cast<sockaddr *>(&peer), &peerLength));
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

    // La destruction de `clients` ferme les connexions encore actives.
    clients.clear();
    closeServerSocket();
    logger_.info("TCP receiver stopped");
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
