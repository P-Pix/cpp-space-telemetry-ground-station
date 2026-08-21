/**
 * @file PortDiagnostics.cpp
 * @brief Implémente les probes TCP loopback et la recherche locale de ports bindables.
 */

#include "stgs/PortDiagnostics.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace stgs
{
    namespace
    {

        // 127.0.0.0/8 est le bloc IPv4 réservé au loopback (RFC 1122). Les valeurs sont exprimées
        // en ordre hôte après ntohl() afin que le masque reste indépendant de l'endianness machine.
        constexpr std::uint32_t Ipv4LoopbackMask = 0xFF000000U;
        constexpr std::uint32_t Ipv4LoopbackPrefix = 0x7F000000U;

        class ScopedFd
        {
        public:
            explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
            ~ScopedFd() { reset(); }
            ScopedFd(const ScopedFd &) = delete;
            ScopedFd &operator=(const ScopedFd &) = delete;
            ScopedFd(ScopedFd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
            ScopedFd &operator=(ScopedFd &&other) noexcept
            {
                if (this != &other)
                {
                    reset();
                    fd_ = other.fd_;
                    other.fd_ = -1;
                }
                return *this;
            }
            [[nodiscard]] int get() const noexcept { return fd_; }
            void reset() noexcept
            {
                if (fd_ >= 0)
                {
                    ::close(fd_);
                    fd_ = -1;
                }
            }

        private:
            int fd_;
        };

        sockaddr_in makeAddress(const std::string &address, std::uint16_t port)
        {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1)
            {
                throw std::runtime_error("invalid IPv4 address: " + address);
            }
            return addr;
        }

        void makeNonBlocking(int fd)
        {
            const int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            {
                throw std::runtime_error("failed to configure non-blocking probe socket");
            }
        }

        void validateDiagnosticPort(std::uint16_t port)
        {
            if (port < MinimumNetworkPort)
            {
                throw std::invalid_argument("diagnostic port must be in the user network-port range");
            }
        }

        void validateDiagnosticRange(PortRange range)
        {
            if (range.first < MinimumNetworkPort || range.last < MinimumNetworkPort || range.first > range.last)
            {
                throw std::invalid_argument("diagnostic port range is invalid");
            }
            if (range.count() > MaxDiagnosticPortCount)
            {
                throw std::invalid_argument("port diagnostic range exceeds MaxDiagnosticPortCount");
            }
        }

    } // namespace

    bool isLoopbackIpv4(const std::string &host) noexcept
    {
        if (host == "localhost")
        {
            return true;
        }
        in_addr address{};
        if (::inet_pton(AF_INET, host.c_str(), &address) != 1)
        {
            return false;
        }
        const auto hostOrder = ntohl(address.s_addr);
        return (hostOrder & Ipv4LoopbackMask) == Ipv4LoopbackPrefix;
    }

    /**
     * @brief Sonde un port TCP loopback avec une connexion non bloquante et un délai borné.
     *
     * Algorithme : `connect()` est lancé sur une socket O_NONBLOCK. Une réussite immédiate classe le
     * port OPEN ; `EINPROGRESS` conduit à attendre `POLLOUT`, puis `SO_ERROR` distingue connexion
     * établie, refus explicite et erreur système. Cette méthode mesure la présence d'un listener TCP,
     * pas l'identité du protocole STGS : aucun handshake applicatif n'existe dans STGS v1.
     *
     * @param host Adresse IPv4 loopback ou `localhost`.
     * @param port Port TCP à tester.
     * @param timeout Durée maximale de l'attente de connexion.
     * @return État OPEN/CLOSED/TIMEOUT/ERROR et latence observée.
     * @throws std::invalid_argument Si la cible n'est pas loopback ou si le timeout est invalide.
     */
    TcpPortProbeResult probeTcpPort(const std::string &host,
                                    std::uint16_t port,
                                    std::chrono::milliseconds timeout)
    {
        if (!isLoopbackIpv4(host))
        {
            throw std::invalid_argument("port diagnostics are intentionally restricted to IPv4 loopback");
        }
        validateDiagnosticPort(port);
        if (timeout.count() <= 0 || timeout > MaximumPortProbeTimeout)
        {
            throw std::invalid_argument("port probe timeout must be in the 1..60000 ms range");
        }
        const std::string normalizedHost = host == "localhost" ? "127.0.0.1" : host;
        TcpPortProbeResult result;
        result.port = port;
        const auto start = std::chrono::steady_clock::now();

        ScopedFd socketFd(::socket(AF_INET, SOCK_STREAM, 0));
        if (socketFd.get() < 0)
        {
            result.state = TcpPortState::Error;
            result.detail = "socket(): " + std::string(std::strerror(errno));
            return result;
        }

        try
        {
            makeNonBlocking(socketFd.get());
            const auto addr = makeAddress(normalizedHost, port);
            const int connectResult = ::connect(socketFd.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
            if (connectResult == 0)
            {
                result.state = TcpPortState::Open;
                result.detail = "TCP listener accepted the connection";
            }
            else if (errno == EINPROGRESS)
            {
                pollfd pfd{socketFd.get(), POLLOUT, 0};
                const int rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
                if (rc == 0)
                {
                    result.state = TcpPortState::Timeout;
                    result.detail = "connection attempt timed out";
                }
                else if (rc < 0)
                {
                    result.state = TcpPortState::Error;
                    result.detail = "poll(): " + std::string(std::strerror(errno));
                }
                else
                {
                    int socketError = 0;
                    socklen_t errorLength = sizeof(socketError);
                    if (::getsockopt(socketFd.get(), SOL_SOCKET, SO_ERROR, &socketError, &errorLength) < 0)
                    {
                        result.state = TcpPortState::Error;
                        result.detail = "getsockopt(SO_ERROR): " + std::string(std::strerror(errno));
                    }
                    else if (socketError == 0)
                    {
                        result.state = TcpPortState::Open;
                        result.detail = "TCP listener accepted the connection";
                    }
                    else if (socketError == ECONNREFUSED)
                    {
                        result.state = TcpPortState::Closed;
                        result.detail = "connection refused";
                    }
                    else
                    {
                        result.state = TcpPortState::Error;
                        result.detail = std::strerror(socketError);
                    }
                }
            }
            else if (errno == ECONNREFUSED)
            {
                result.state = TcpPortState::Closed;
                result.detail = "connection refused";
            }
            else
            {
                result.state = TcpPortState::Error;
                result.detail = std::strerror(errno);
            }
        }
        catch (const std::exception &ex)
        {
            result.state = TcpPortState::Error;
            result.detail = ex.what();
        }

        const auto end = std::chrono::steady_clock::now();
        result.latencyMs = std::chrono::duration<double, std::milli>(end - start).count();
        return result;
    }

    /**
     * @brief Exécute une série de probes TCP séquentielles sur une plage loopback bornée.
     *
     * Le parcours reste volontairement séquentiel : l'outil privilégie un rapport lisible et une
     * charge locale prévisible plutôt que la vitesse d'un scanner parallèle. La limite
     * MaxDiagnosticPortCount empêche également qu'une erreur de CLI déclenche des milliers de probes.
     */
    std::vector<TcpPortProbeResult> scanTcpPorts(const std::string &host,
                                                 PortRange range,
                                                 std::chrono::milliseconds timeout)
    {
        if (!isLoopbackIpv4(host))
        {
            throw std::invalid_argument("port scan is intentionally restricted to IPv4 loopback");
        }
        validateDiagnosticRange(range);
        std::vector<TcpPortProbeResult> results;
        results.reserve(range.count());
        for (std::uint32_t port = range.first; port <= range.last; ++port)
        {
            results.push_back(probeTcpPort(host, static_cast<std::uint16_t>(port), timeout));
        }
        return results;
    }

    LocalPortAvailability checkLocalPortAvailability(const std::string &bindAddress,
                                                     std::uint16_t port,
                                                     PortTransport transport)
    {
        validateDiagnosticPort(port);
        LocalPortAvailability result;
        result.port = port;
        const int socketType = transport == PortTransport::Tcp ? SOCK_STREAM : SOCK_DGRAM;
        ScopedFd socketFd(::socket(AF_INET, socketType, 0));
        if (socketFd.get() < 0)
        {
            result.detail = "socket(): " + std::string(std::strerror(errno));
            return result;
        }

        try
        {
            const auto addr = makeAddress(bindAddress, port);
            if (::bind(socketFd.get(), reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0)
            {
                result.available = true;
                result.detail = "available for bind";
            }
            else if (errno == EADDRINUSE)
            {
                result.detail = "already in use";
            }
            else
            {
                result.detail = "bind(): " + std::string(std::strerror(errno));
            }
        }
        catch (const std::exception &ex)
        {
            result.detail = ex.what();
        }
        return result;
    }

    std::optional<std::uint16_t> findFirstAvailableLocalPort(const std::string &bindAddress,
                                                             PortRange range,
                                                             PortTransport transport,
                                                             std::vector<LocalPortAvailability> *report)
    {
        validateDiagnosticRange(range);
        if (report != nullptr)
        {
            report->clear();
            report->reserve(range.count());
        }
        for (std::uint32_t port = range.first; port <= range.last; ++port)
        {
            auto result = checkLocalPortAvailability(bindAddress, static_cast<std::uint16_t>(port), transport);
            const bool available = result.available;
            if (report != nullptr)
            {
                report->push_back(result);
            }
            if (available)
            {
                return static_cast<std::uint16_t>(port);
            }
        }
        return std::nullopt;
    }

    const char *tcpPortStateToString(TcpPortState state) noexcept
    {
        switch (state)
        {
        case TcpPortState::Open:
            return "OPEN";
        case TcpPortState::Closed:
            return "CLOSED";
        case TcpPortState::Timeout:
            return "TIMEOUT";
        case TcpPortState::Error:
            return "ERROR";
        }
        return "UNKNOWN";
    }

} // namespace stgs
