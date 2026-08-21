/**
 * @file GroundStationOptions.cpp
 * @brief Parse et valide la configuration de la station sol.
 *
 * Les contrôles sont centralisés ici afin que le reste de l'application puisse supposer que les
 * modes, bornes numériques et seuils d'hystérésis sont cohérents avant tout effet de bord.
 */
#include "GroundStationOptions.hpp"

#include "stgs/ApplicationPayload.hpp"
#include "stgs/CliParsing.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace stgs::app::ground_station
{

    namespace
    {

        // Garde-fous CLI : ils protègent contre une saisie accidentelle disproportionnée et ne sont pas
        // des limites du protocole STGS lui-même.
        inline constexpr std::size_t MaximumDecoderThreads = 256U;
        inline constexpr std::size_t MaximumQueueCapacity = 1'000'000U;
        inline constexpr std::size_t MaximumHealthWindowSamples = 1'000'000U;

        // Un filtre centré doit être impair et n'a aucun intérêt à dépasser le plus grand SIGNAL_BLOCK.
        inline constexpr std::size_t MaximumFilterWindow =
            (MaxSignalSamplesPerPayload % 2U) == 0U
                ? MaxSignalSamplesPerPayload - 1U
                : MaxSignalSamplesPerPayload;

        std::string requireValue(int &index, int argc, char **argv)
        {
            if (index + 1 >= argc)
            {
                throw std::runtime_error(std::string("missing value after ") + argv[index]);
            }
            ++index;
            return argv[index];
        }

        void selectTransport(Options &options, Transport transport)
        {
            if (options.transport.has_value() && *options.transport != transport)
            {
                throw std::runtime_error("--udp and --tcp are mutually exclusive");
            }
            options.transport = transport;
        }

        OutputFormat parseOutputFormat(const std::string &value)
        {
            if (value == "csv")
            {
                return OutputFormat::Csv;
            }
            if (value == "json")
            {
                return OutputFormat::Json;
            }
            throw std::runtime_error("--output-format must be csv or json");
        }

        SignalFilterMode parseSignalFilter(const std::string &value)
        {
            if (value == "none")
            {
                return SignalFilterMode::None;
            }
            if (value == "moving-average")
            {
                return SignalFilterMode::MovingAverage;
            }
            if (value == "sine-projection")
            {
                return SignalFilterMode::SineProjection;
            }
            throw std::runtime_error("--signal-filter must be none, moving-average or sine-projection");
        }

    } // namespace

    OutputFormat resolveOutputFormat(OutputFormat requested, const std::filesystem::path &output)
    {
        if (requested != OutputFormat::Auto)
        {
            return requested;
        }
        return output.extension() == ".json" ? OutputFormat::Json : OutputFormat::Csv;
    }

    Options parseArgs(int argc, char **argv)
    {
        Options options;
        bool portExplicit = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h")
            {
                printUsage();
                std::exit(0);
            }
            else if (arg == "--udp")
            {
                selectTransport(options, Transport::Udp);
            }
            else if (arg == "--tcp")
            {
                selectTransport(options, Transport::Tcp);
            }
            else if (arg == "--bind")
            {
                options.bindAddress = requireValue(i, argc, argv);
            }
            else if (arg == "--port")
            {
                options.port = parseUnsigned<std::uint16_t>(
                    requireValue(i, argc, argv), "--port", MinimumNetworkPort, MaximumNetworkPort);
                portExplicit = true;
            }
            else if (arg == "--auto-port")
            {
                options.autoPortRange = parsePortRange(requireValue(i, argc, argv), "--auto-port");
            }
            else if (arg == "--replay")
            {
                options.replayFile = requireValue(i, argc, argv);
            }
            else if (arg == "--replay-rate")
            {
                options.replayRate = parseFiniteDouble(requireValue(i, argc, argv), "--replay-rate");
                if (options.replayRate < 0.0)
                {
                    throw std::runtime_error("--replay-rate must be >= 0");
                }
            }
            else if (arg == "--output")
            {
                options.outputFile = requireValue(i, argc, argv);
            }
            else if (arg == "--output-format")
            {
                options.outputFormat = parseOutputFormat(requireValue(i, argc, argv));
            }
            else if (arg == "--log")
            {
                options.logFile = requireValue(i, argc, argv);
            }
            else if (arg == "--log-level")
            {
                const auto level = parseLogLevel(requireValue(i, argc, argv));
                if (!level.has_value())
                {
                    throw std::runtime_error("unknown log level");
                }
                options.logLevel = *level;
            }
            else if (arg == "--verbose")
            {
                options.verbose = true;
                options.logLevel = LogLevel::Debug;
            }
            else if (arg == "--decoder-threads")
            {
                options.decoderThreads = parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--decoder-threads", 1U, MaximumDecoderThreads);
            }
            else if (arg == "--queue-capacity")
            {
                options.queueCapacity = parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--queue-capacity", 1U, MaximumQueueCapacity);
            }
            else if (arg == "--signal-filter")
            {
                options.signalFilter = parseSignalFilter(requireValue(i, argc, argv));
            }
            else if (arg == "--filter-window")
            {
                options.movingAverageWindow = parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--filter-window", 1U, MaximumFilterWindow);
            }
            else if (arg == "--disable-degraded")
            {
                options.healthConfig.enabled = false;
            }
            else if (arg == "--degraded-window")
            {
                options.healthConfig.windowSize = parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--degraded-window", 1U, MaximumHealthWindowSamples);
            }
            else if (arg == "--degraded-min-samples")
            {
                options.healthConfig.minSamples = parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--degraded-min-samples", 1U, MaximumHealthWindowSamples);
            }
            else if (arg == "--degraded-rejection-rate")
            {
                options.healthConfig.degradedRejectionRate =
                    parseProbability(requireValue(i, argc, argv), "--degraded-rejection-rate");
            }
            else if (arg == "--degraded-recovery-rate")
            {
                options.healthConfig.recoveryRejectionRate =
                    parseProbability(requireValue(i, argc, argv), "--degraded-recovery-rate");
            }
            else if (arg == "--degraded-critical-count")
            {
                options.healthConfig.criticalFramesForDegraded = parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--degraded-critical-count", 1U, MaximumHealthWindowSamples);
            }
            else if (arg == "--recovery-critical-count")
            {
                options.healthConfig.criticalFramesForRecovery = parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--recovery-critical-count", 0U, MaximumHealthWindowSamples);
            }
            else if (arg == "--no-color")
            {
                options.color = false;
            }
            else
            {
                throw std::runtime_error("unknown option: " + arg);
            }
        }

        if (options.replayFile.has_value() && options.transport.has_value())
        {
            throw std::runtime_error("choose either network mode or replay mode, not both");
        }
        if (!options.replayFile.has_value() && !options.transport.has_value())
        {
            throw std::runtime_error("choose --udp, --tcp, or --replay");
        }
        if (options.autoPortRange.has_value())
        {
            if (!options.transport.has_value() || *options.transport != Transport::Tcp)
            {
                throw std::runtime_error("--auto-port requires --tcp");
            }
            if (portExplicit)
            {
                throw std::runtime_error("choose either --port or --auto-port, not both");
            }
            if (options.autoPortRange->count() > MaxDiagnosticPortCount)
            {
                throw std::runtime_error("--auto-port exceeds MaxDiagnosticPortCount");
            }
        }
        if ((options.movingAverageWindow % 2U) == 0U)
        {
            throw std::runtime_error("--filter-window must be odd for a centered moving average");
        }
        if (options.healthConfig.minSamples > options.healthConfig.windowSize)
        {
            throw std::runtime_error("--degraded-min-samples cannot exceed --degraded-window");
        }
        if (options.healthConfig.criticalFramesForRecovery >= options.healthConfig.criticalFramesForDegraded)
        {
            throw std::runtime_error("recovery critical threshold must be lower than degradation threshold");
        }
        return options;
    }

} // namespace stgs::app::ground_station
