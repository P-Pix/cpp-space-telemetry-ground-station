/**
 * @file NetworkServer.hpp
 * @brief Déclare le récepteur réseau UDP/TCP de la station sol.
 *
 * Le composant transforme le transport réseau en candidats de trame complets. Il ne valide pas le
 * CRC et n'interprète pas la télémétrie : ces responsabilités restent dans FrameCodec et dans la
 * couche applicative, ce qui permet de tester le framing indépendamment des règles métier.
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

    /** Port non privilégié choisi comme valeur de démonstration par défaut pour STGS. */
    inline constexpr std::uint16_t DefaultTelemetryPort = 9000U;
    /** Compromis de démonstration entre arrêt réactif et réveils périodiques de poll(). */
    inline constexpr int DefaultNetworkPollTimeoutMs = 250;

    /** @brief Transports IPv4 supportés par la station de démonstration. */
    enum class Transport
    {
        Udp, ///< Un datagramme correspond à un candidat de trame indivisible.
        Tcp  ///< Un flux est réassemblé par StreamFrameExtractor pour chaque client.
    };

    /**
     * @brief Paramètres de la boucle de réception réseau.
     *
     * `pollTimeoutMs` borne le délai avant réévaluation de l'indicateur d'arrêt. La valeur par
     * défaut de 250 ms est un compromis de démonstration entre réactivité à Ctrl+C et réveils CPU.
     */
    struct NetworkConfig
    {
        Transport transport = Transport::Udp;
        std::string bindAddress = "0.0.0.0";
        std::uint16_t port = DefaultTelemetryPort;
        int pollTimeoutMs = DefaultNetworkPollTimeoutMs;
    };

    /**
     * @brief Reçoit des candidats de trame sur UDP ou TCP puis les remet au pipeline applicatif.
     *
     * Objectif projet :
     * Isoler sockets, `poll()`, multi-clients TCP et réassemblage de flux afin que le pipeline de
     * décodage n'ait pas à connaître les différences entre datagrammes UDP et flux TCP.
     *
     * Garanties importantes :
     * - un datagramme UDP surdimensionné est rejeté au lieu d'être traité tronqué ;
     * - chaque client TCP possède son propre StreamFrameExtractor ;
     * - les frontières de `recv()` ne sont jamais assimilées à des frontières de trame ;
     * - les octets disponibles sont drainés lors d'un `POLLHUP` avant fermeture du client.
     */
    class NetworkServer
    {
    public:
        using FrameCallback = std::function<void(ByteVector)>;

        /**
         * @brief Construit un serveur réseau sans ouvrir immédiatement de socket.
         * @param config Transport, adresse, port et timeout de polling.
         * @param logger Logger partagé avec l'application.
         * @throws std::invalid_argument Si le port ou le timeout sont invalides.
         */
        NetworkServer(NetworkConfig config, Logger &logger);
        /** @brief Garantit la fermeture de la socket serveur même après une sortie exceptionnelle. */
        ~NetworkServer();

        NetworkServer(const NetworkServer &) = delete;
        NetworkServer &operator=(const NetworkServer &) = delete;

        /**
         * @brief Exécute la boucle de réception correspondant au transport configuré.
         * @param callback Fonction appelée pour chaque candidat de trame wire complet.
         * @param running Indicateur externe permettant à l'application de demander l'arrêt.
         * @throws std::runtime_error Si l'initialisation réseau ou `poll()` échoue.
         */
        void run(FrameCallback callback, const std::atomic_bool &running);

        /**
         * @brief Demande un arrêt coopératif de la boucle réseau depuis un autre thread.
         *
         * La méthode ne ferme volontairement pas le descripteur serveur depuis le thread appelant :
         * fermer un fd pendant qu'un autre thread l'utilise dans `poll()` introduirait des problèmes
         * de durée de vie et de réutilisation de descripteur. L'atomic `stopRequested_` est relu au
         * plus tard après `pollTimeoutMs`, puis le thread réseau ferme lui-même ses sockets.
         *
         * La méthode est idempotente et thread-safe tant que l'objet NetworkServer reste vivant.
         */
        void stop() noexcept;

    private:
        /** @brief Boucle UDP : un datagramme complet correspond à un candidat de trame. */
        void runUdp(FrameCallback &callback, const std::atomic_bool &running);

        /** @brief Boucle TCP multi-clients : poll, drain des sockets et framing par connexion. */
        void runTcp(FrameCallback &callback, const std::atomic_bool &running);

        /** @brief Crée puis bind une socket IPv4 en activant SO_REUSEADDR. */
        int createBoundSocket(int socketType) const;

        /** @brief Ferme la socket serveur si elle est ouverte. */
        void closeServerSocket() noexcept;

        NetworkConfig config_;
        Logger &logger_;
        int serverFd_ = -1;
        std::atomic_bool stopRequested_{false};
    };

    /** @brief Convertit le transport en libellé stable pour logs et interface terminal. */
    std::string transportToString(Transport transport);

} // namespace stgs
