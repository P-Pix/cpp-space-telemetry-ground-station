/**
 * @file NetworkServerUdp.cpp
 * @brief Implémente la réception UDP des trames STGS.
 */
#include "stgs/NetworkServer.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>

namespace stgs
{

    /**
     * @brief Reçoit des datagrammes UDP complets sans accepter silencieusement une troncature.
     *
     * Le buffer fait exactement `MaxFrameSize` et `MSG_TRUNC` demande au noyau de retourner la taille
     * réelle du datagramme. Une taille supérieure est donc identifiable et rejetée au lieu de présenter
     * au codec un préfixe tronqué. `poll()` conserve un arrêt réactif sans boucle active.
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

} // namespace stgs
