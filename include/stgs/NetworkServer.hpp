/**
 * @file NetworkServer.hpp
 * @brief Déclare le récepteur réseau UDP/TCP de la station sol.
 *
 * Le composant livre uniquement des candidats de trame complets au pipeline ; le décodage et les règles de santé restent hors de la couche réseau.
 */

#pragma once

#include "stgs/FrameCodec.hpp"
#include "stgs/Logger.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace stgs
{

    enum class Transport
    {
        Udp,
        Tcp
    };

    struct NetworkConfig
    {
        Transport transport = Transport::Udp;
        std::string bindAddress = "0.0.0.0";
        std::uint16_t port = 9000;
        int pollTimeoutMs = 250;
    };

    /**
     * @brief Reçoit des candidats de trame sur UDP ou TCP puis les remet au pipeline applicatif.
     *
     * La couche réseau ne décode pas la télémétrie : elle gère sockets, clients TCP et framing,
     * puis appelle FrameCallback. Cette séparation garde les validations binaires dans FrameCodec.
     */
    class NetworkServer
    {
    public:
        using FrameCallback = std::function<void(ByteVector)>;

        NetworkServer(NetworkConfig config, Logger &logger);
        ~NetworkServer();

        NetworkServer(const NetworkServer &) = delete;
        NetworkServer &operator=(const NetworkServer &) = delete;

        /**
         * @brief Exécute la boucle de réception correspondant au transport configuré.
         * @param callback Fonction appelée pour chaque candidat de trame complet.
         * @param running Indicateur externe permettant à l’application d’arrêter la réception.
         * @throws std::exception Si l’initialisation réseau échoue.
         */
        void run(FrameCallback callback, const std::atomic_bool &running);

        /**
         * @brief Signale l’arrêt et ferme la socket serveur afin de réveiller les attentes réseau.
         */
        void stop();

    private:
        void runUdp(FrameCallback &callback, const std::atomic_bool &running);
        void runTcp(FrameCallback &callback, const std::atomic_bool &running);
        int createBoundSocket(int socketType) const;
        void closeServerSocket() noexcept;

        NetworkConfig config_;
        Logger &logger_;
        int serverFd_ = -1;
        std::atomic_bool stopRequested_{false};
    };

    std::string transportToString(Transport transport);

} // namespace stgs
