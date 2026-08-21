/**
 * @file GroundStationOptions.hpp
 * @brief Définit la configuration CLI validée de la station sol.
 *
 * Ce header isole le contrat de configuration du pipeline d'exécution. Les autres modules de
 * l'application consomment un objet `Options` déjà cohérent et n'ont pas à connaître la syntaxe
 * exacte des arguments de ligne de commande.
 */
#pragma once

#include "stgs/Logger.hpp"
#include "stgs/NetworkServer.hpp"
#include "stgs/PortDiagnostics.hpp"
#include "stgs/SignalProcessing.hpp"
#include "stgs/StationHealth.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace stgs::app::ground_station
{
    enum class OutputFormat
    {
        Auto,
        Csv,
        Json
    };

    /**
     * @brief Regroupe les options de la station après parsing et validation croisée.
     *
     * Les invariants entre modes (réseau/replay, TCP/UDP, port explicite/automatique), tailles de
     * files, nombre de workers, seuils de santé et paramètres de filtrage sont contrôlés avant que
     * l'application n'ouvre un fichier, ne crée un thread ou n'écoute une socket.
     */
    struct Options
    {
        std::optional<Transport> transport;
        std::string bindAddress = "0.0.0.0";
        std::uint16_t port = DefaultTelemetryPort;
        std::optional<PortRange> autoPortRange;
        std::optional<std::filesystem::path> replayFile;
        double replayRate = 0.0;
        std::filesystem::path outputFile = "telemetry.csv";
        OutputFormat outputFormat = OutputFormat::Auto;
        std::optional<std::filesystem::path> logFile;
        LogLevel logLevel = LogLevel::Info;
        std::size_t decoderThreads = 2U;
        std::size_t queueCapacity = 1024U;
        StationHealthConfig healthConfig;
        SignalFilterMode signalFilter = SignalFilterMode::SineProjection;
        std::size_t movingAverageWindow = 5U;
        bool color = true;
        bool verbose = false;
    };

    /** @brief Affiche l'aide de la station sol sur stdout. */
    void printUsage();

    /**
     * @brief Parse et valide l'ensemble de la configuration de la station avant tout effet de bord.
     * @param argc Nombre d'arguments transmis au processus.
     * @param argv Tableau des arguments C.
     * @return Options cohérentes prêtes à être appliquées par l'application.
     * @throws std::runtime_error Pour toute option inconnue, valeur invalide ou combinaison interdite.
     */
    Options parseArgs(int argc, char **argv);

    /**
     * @brief Résout le format d'export effectif à partir de l'option et de l'extension du fichier.
     * @param requested Format explicitement demandé ou `Auto`.
     * @param output Chemin de sortie utilisé pour l'inférence éventuelle.
     * @return Format CSV ou JSON réellement utilisé.
     */
    OutputFormat resolveOutputFormat(OutputFormat requested, const std::filesystem::path &output);

} // namespace stgs::app::ground_station
