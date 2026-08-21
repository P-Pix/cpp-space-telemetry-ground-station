/**
 * @file PortDiagnostics.hpp
 * @brief Déclare les diagnostics de ports locaux utilisés par les outils STGS.
 *
 * Cette API est volontairement limitée au loopback IPv4 et à une plage courte. Elle sert à
 * diagnostiquer une maquette exécutée sur le poste du développeur : elle ne constitue ni un scanner
 * réseau généraliste ni une découverte de service distante.
 */

#pragma once

#include "stgs/CliParsing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace stgs
{

    /** Nombre maximal de ports testés en une commande pour garder le diagnostic local et borné. */
    inline constexpr std::size_t MaxDiagnosticPortCount = 256U;
    /** Timeout court par défaut : le diagnostic ne cible que la boucle locale. */
    inline constexpr std::chrono::milliseconds DefaultPortProbeTimeout{200};
    /** Borne CLI défensive évitant qu'une faute de saisie bloque un probe pendant plusieurs minutes. */
    inline constexpr std::chrono::milliseconds MaximumPortProbeTimeout{60'000};

    /** @brief Type de socket utilisé lors d'un test local de disponibilité pour `bind()`. */
    enum class PortTransport
    {
        Tcp, ///< Vérifie la disponibilité d'un port SOCK_STREAM local.
        Udp  ///< Vérifie la disponibilité d'un port SOCK_DGRAM local.
    };

    /** @brief Résultat observable d'un probe TCP non bloquant sur le loopback. */
    enum class TcpPortState
    {
        Open,    ///< Une connexion TCP a été acceptée ; l'identité STGS n'est pas affirmée.
        Closed,  ///< Le noyau local a refusé la connexion.
        Timeout, ///< Aucun résultat n'a été obtenu avant le délai borné.
        Error    ///< Une autre erreur socket/adresse a empêché le diagnostic.
    };

    /** @brief Résultat d'une tentative de connexion TCP locale non bloquante. */
    struct TcpPortProbeResult
    {
        std::uint16_t port = 0U;
        TcpPortState state = TcpPortState::Error;
        double latencyMs = 0.0;
        std::string detail;
    };

    /** @brief Résultat d'un test de disponibilité pour un futur `bind()` local. */
    struct LocalPortAvailability
    {
        std::uint16_t port = 0U;
        bool available = false;
        std::string detail;
    };

    /**
     * @brief Indique si une adresse IPv4 textuelle cible le loopback 127.0.0.0/8.
     * @param host `localhost` ou adresse IPv4 numérique.
     * @return true uniquement pour une cible locale loopback.
     */
    bool isLoopbackIpv4(const std::string &host) noexcept;

    /**
     * @brief Tente une connexion TCP locale non bloquante et mesure sa latence.
     *
     * Un état OPEN confirme qu'un listener TCP a accepté la connexion ; il ne prouve pas que le
     * service est STGS, car le protocole de télémétrie reste volontairement unidirectionnel.
     *
     * @param host Adresse loopback (`127.x.x.x` ou `localhost`).
     * @param port Port cible.
     * @param timeout Durée maximale du probe.
     * @return État OPEN/CLOSED/TIMEOUT/ERROR et détail opérateur.
     * @throws std::invalid_argument Si la cible n'est pas loopback, si le port vaut 0 ou si le
     * timeout est invalide.
     */
    TcpPortProbeResult probeTcpPort(const std::string &host,
                                    std::uint16_t port,
                                    std::chrono::milliseconds timeout);

    /**
     * @brief Probe séquentiellement une plage locale inclusive et produit un rapport par port.
     * @param host Cible loopback uniquement.
     * @param range Plage inclusive, limitée à MaxDiagnosticPortCount entrées.
     * @param timeout Timeout individuel de connexion.
     * @return Résultats dans l'ordre croissant des ports.
     * @throws std::invalid_argument Si la plage est invalide/trop grande ou la cible non locale.
     */
    std::vector<TcpPortProbeResult> scanTcpPorts(const std::string &host,
                                                 PortRange range,
                                                 std::chrono::milliseconds timeout);

    /**
     * @brief Vérifie si une socket locale peut être bindée sur un port sans conserver la socket.
     * @param bindAddress Adresse IPv4 locale utilisée par le futur serveur.
     * @param port Port candidat.
     * @param transport Type de socket à tester.
     * @return Disponibilité et diagnostic de `bind()`.
     */
    LocalPortAvailability checkLocalPortAvailability(const std::string &bindAddress,
                                                     std::uint16_t port,
                                                     PortTransport transport);

    /**
     * @brief Cherche le premier port local bindable d'une plage inclusive.
     *
     * Objectif projet : permettre `--auto-port` côté station sans choisir silencieusement un port
     * arbitraire. Si `report` est fourni, chaque port testé reste visible dans le terminal.
     *
     * @param bindAddress Adresse sur laquelle le serveur écoutera réellement.
     * @param range Plage de candidats, limitée à MaxDiagnosticPortCount.
     * @param transport TCP ou UDP.
     * @param report Destination facultative du rapport détaillé.
     * @return Premier port libre ou std::nullopt si aucun candidat ne convient.
     * @throws std::invalid_argument Si la plage est invalide ou dépasse MaxDiagnosticPortCount.
     */
    std::optional<std::uint16_t> findFirstAvailableLocalPort(
        const std::string &bindAddress,
        PortRange range,
        PortTransport transport,
        std::vector<LocalPortAvailability> *report = nullptr);

    /** @brief Convertit l'état d'un probe en libellé stable pour le terminal. */
    const char *tcpPortStateToString(TcpPortState state) noexcept;

} // namespace stgs
