/**
 * @file SimulatorSocket.hpp
 * @brief Déclare les primitives réseau utilisées par le simulateur.
 */
#pragma once

#include "SimulatorOptions.hpp"
#include "stgs/TelemetryFrame.hpp"

#include <netinet/in.h>

namespace stgs::app::simulator
{

    /** @brief Petit propriétaire RAII d'un descripteur de socket POSIX. */
    class SocketFd
    {
    public:
        explicit SocketFd(int fd = -1) noexcept;
        ~SocketFd();

        SocketFd(const SocketFd &) = delete;
        SocketFd &operator=(const SocketFd &) = delete;
        SocketFd(SocketFd &&other) noexcept;
        SocketFd &operator=(SocketFd &&other) noexcept;

        [[nodiscard]] int get() const noexcept;

    private:
        int fd_ = -1;
    };

    /** @brief Crée une socket UDP ou TCP correspondant au transport demandé. */
    SocketFd createSocket(Transport transport);

    /**
     * @brief Construit l'adresse IPv4 de destination à partir de la configuration.
     * @throws std::runtime_error Si l'hôte n'est pas une adresse IPv4 exploitable.
     */
    sockaddr_in destination(const Options &options);

    /**
     * @brief Envoie exactement tous les octets d'une trame sur une socket TCP.
     *
     * TCP autorise les écritures partielles : la fonction boucle jusqu'à `bytes.size()` et utilise
     * `MSG_NOSIGNAL` pour transformer une fermeture distante en exception plutôt qu'en SIGPIPE.
     */
    void sendAll(int fd, const ByteVector &bytes);

} // namespace stgs::app::simulator
