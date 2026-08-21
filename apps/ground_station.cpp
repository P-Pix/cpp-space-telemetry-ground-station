/**
 * @file ground_station.cpp
 * @brief Orchestre la station sol, son pipeline multithread et l'affichage opérateur.
 *
 * Le point d'entrée relie réseau/replay, files bornées, workers de décodage, réordonnancement,
 * suivi de santé, interprétation des payloads applicatifs STGA et export CSV/JSON. Les résultats
 * sont remis dans l'ordre de réception avant toute règle d'état ou écriture afin que le nombre de
 * threads n'altère jamais le comportement fonctionnel.
 */

#include "stgs/ApplicationPayload.hpp"
#include "stgs/BlockingQueue.hpp"
#include "stgs/CliParsing.hpp"
#include "stgs/FrameCodec.hpp"
#include "stgs/Logger.hpp"
#include "stgs/NetworkServer.hpp"
#include "stgs/PortDiagnostics.hpp"
#include "stgs/Replay.hpp"
#include "stgs/SignalProcessing.hpp"
#include "stgs/StationHealth.hpp"
#include "stgs/TerminalUi.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <pthread.h>
#include <signal.h>

namespace
{

    // Valeurs de démonstration/CLI nommées pour distinguer clairement configuration et logique.
    inline constexpr std::size_t DefaultDecoderThreads = 2U;
    inline constexpr std::size_t DefaultQueueCapacity = 1024U;
    inline constexpr std::size_t DefaultMovingAverageWindow = 5U;
    // Garde-fous CLI : ils empêchent une saisie accidentelle de créer des milliers de threads ou
    // des allocations gigantesques. Ils ne constituent pas des limites du protocole STGS.
    inline constexpr std::size_t MaximumDecoderThreads = 256U;
    inline constexpr std::size_t MaximumQueueCapacity = 1'000'000U;
    inline constexpr std::size_t MaximumHealthWindowSamples = 1'000'000U;
    // Un filtre centré doit être impair et n'a pas d'intérêt à dépasser le plus grand SIGNAL_BLOCK.
    inline constexpr std::size_t MaximumFilterWindow =
        (stgs::MaxSignalSamplesPerPayload % 2U) == 0U
            ? stgs::MaxSignalSamplesPerPayload - 1U
            : stgs::MaxSignalSamplesPerPayload;
    // JSON impose l'échappement des codes C0 U+0000..U+001F sous forme \uXXXX.
    inline constexpr unsigned char JsonControlUpperExclusive = 0x20U;
    inline constexpr int JsonUnicodeEscapeHexWidth = 4;
    inline constexpr std::chrono::seconds PipelineProgressInterval{1};

    // Une attente courte garde l'arrêt normal réactif sans exécuter de code C++ dans un handler POSIX.
    inline constexpr long SignalWaitPollNanoseconds = 100'000'000L;

    /**
     * @brief Convertit SIGINT/SIGTERM en demande d'arrêt depuis un thread ordinaire.
     *
     * Objectif projet :
     * éviter un gestionnaire de signal asynchrone qui appellerait directement des primitives C++
     * dont l'async-signal-safety n'est pas garantie par POSIX. Le thread principal bloque SIGINT et
     * SIGTERM avant la création des workers ; ceux-ci héritent donc du masque. Un `std::jthread`
     * dédié consomme ensuite les signaux avec `sigtimedwait()` et peut modifier l'atomic `running`
     * dans un contexte d'exécution normal.
     *
     * Fonctionnement :
     * 1. construit l'ensemble {SIGINT, SIGTERM} ;
     * 2. bloque cet ensemble dans le thread appelant et sauvegarde son masque précédent ;
     * 3. attend les signaux de manière synchrone dans un thread dédié ;
     * 4. positionne `running` à false lorsqu'un arrêt opérateur est reçu ;
     * 5. restaure le masque initial après l'arrêt du thread d'attente.
     *
     * Cette stratégie conserve un arrêt Ctrl+C propre tout en évitant d'effectuer journalisation,
     * allocation, verrouillage ou opération de bibliothèque C++ depuis un handler asynchrone.
     */
    class PosixSignalStopController
    {
    public:
        /**
         * @brief Bloque SIGINT/SIGTERM et démarre le thread chargé de les attendre.
         * @param running Drapeau partagé pilotant les boucles réseau et replay.
         * @throws std::system_error Si l'installation du masque ou l'attente POSIX échoue.
         */
        explicit PosixSignalStopController(std::atomic_bool &running)
            : running_(running)
        {
            if (::sigemptyset(&signalSet_) != 0 ||
                ::sigaddset(&signalSet_, SIGINT) != 0 ||
                ::sigaddset(&signalSet_, SIGTERM) != 0)
            {
                throw std::system_error(errno, std::generic_category(), "failed to build POSIX signal set");
            }

            const int maskResult = ::pthread_sigmask(SIG_BLOCK, &signalSet_, &previousMask_);
            if (maskResult != 0)
            {
                throw std::system_error(maskResult, std::generic_category(), "pthread_sigmask(SIG_BLOCK)");
            }
            maskInstalled_ = true;

            try
            {
                waiter_ = std::jthread([this](std::stop_token stopToken)
                {
                    waitLoop(stopToken);
                });
            }
            catch (...)
            {
                restoreMaskNoThrow();
                throw;
            }
        }

        PosixSignalStopController(const PosixSignalStopController &) = delete;
        PosixSignalStopController &operator=(const PosixSignalStopController &) = delete;

        /** @brief Arrête le thread d'attente et restaure toujours le masque POSIX initial. */
        ~PosixSignalStopController()
        {
            stopNoThrow();
        }

        /**
         * @brief Finalise explicitement le contrôleur et relaie une éventuelle erreur d'attente.
         *
         * L'appel explicite permet au chemin nominal de ne pas masquer une panne rare de
         * `sigtimedwait()`. Le destructeur reste `noexcept` par construction et sert de filet de
         * sécurité lorsque la pile est déroulée par une autre exception.
         *
         * @throws std::system_error Si le thread d'attente a rencontré une erreur POSIX.
         */
        void finish()
        {
            stopNoThrow();
            std::lock_guard<std::mutex> lock(errorMutex_);
            if (error_ != nullptr)
            {
                std::rethrow_exception(error_);
            }
        }

    private:
        /**
         * @brief Attend les signaux bloqués sans handler asynchrone et avec arrêt coopératif.
         *
         * `sigtimedwait()` est préféré à `sigwait()` afin que `std::jthread::request_stop()` puisse
         * terminer la boucle même lorsqu'aucun signal opérateur n'arrive. EAGAIN signifie seulement
         * que la tranche d'attente est écoulée ; EINTR est également retenté. Toute autre erreur est
         * mémorisée puis transforme le pipeline en arrêt contrôlé.
         */
        void waitLoop(std::stop_token stopToken) noexcept
        {
            while (!stopToken.stop_requested())
            {
                const ::timespec timeout{0, SignalWaitPollNanoseconds};
                errno = 0;
                const int signalNumber = ::sigtimedwait(&signalSet_, nullptr, &timeout);
                if (signalNumber == SIGINT || signalNumber == SIGTERM)
                {
                    running_.store(false);
                    return;
                }
                if (signalNumber == -1 && (errno == EAGAIN || errno == EINTR))
                {
                    continue;
                }
                if (signalNumber == -1)
                {
                    captureError(std::make_exception_ptr(
                        std::system_error(errno, std::generic_category(), "sigtimedwait")));
                    running_.store(false);
                    return;
                }
            }
        }

        /** @brief Conserve la première erreur du thread d'attente sans laisser d'exception s'en échapper. */
        void captureError(std::exception_ptr error) noexcept
        {
            try
            {
                std::lock_guard<std::mutex> lock(errorMutex_);
                if (error_ == nullptr)
                {
                    error_ = std::move(error);
                }
            }
            catch (...)
            {
                // Un mutex standard ne doit pas échouer en fonctionnement normal ; l'arrêt reste prioritaire.
            }
        }

        /** @brief Demande l'arrêt du waiter, le joint puis restaure le masque sans lever d'exception. */
        void stopNoThrow() noexcept
        {
            if (waiter_.joinable())
            {
                waiter_.request_stop();
                waiter_.join();
            }
            restoreMaskNoThrow();
        }

        /** @brief Restaure le masque du thread principal exactement tel qu'il était à la construction. */
        void restoreMaskNoThrow() noexcept
        {
            if (maskInstalled_)
            {
                (void)::pthread_sigmask(SIG_SETMASK, &previousMask_, nullptr);
                maskInstalled_ = false;
            }
        }

        std::atomic_bool &running_;
        ::sigset_t signalSet_{};
        ::sigset_t previousMask_{};
        bool maskInstalled_ = false;
        std::jthread waiter_;
        std::mutex errorMutex_;
        std::exception_ptr error_;
    };

    enum class OutputFormat
    {
        Auto,
        Csv,
        Json
    };

    struct Options
    {
        std::optional<stgs::Transport> transport;
        std::string bindAddress = "0.0.0.0";
        std::uint16_t port = stgs::DefaultTelemetryPort;
        std::optional<stgs::PortRange> autoPortRange;
        std::optional<std::filesystem::path> replayFile;
        double replayRate = 0.0;
        std::filesystem::path outputFile = "telemetry.csv";
        OutputFormat outputFormat = OutputFormat::Auto;
        std::optional<std::filesystem::path> logFile;
        stgs::LogLevel logLevel = stgs::LogLevel::Info;
        std::size_t decoderThreads = DefaultDecoderThreads;
        std::size_t queueCapacity = DefaultQueueCapacity;
        stgs::StationHealthConfig healthConfig;
        stgs::SignalFilterMode signalFilter = stgs::SignalFilterMode::SineProjection;
        std::size_t movingAverageWindow = DefaultMovingAverageWindow;
        bool color = true;
        bool verbose = false;
    };

    struct RawWorkItem
    {
        std::uint64_t sequence = 0U;
        stgs::ByteVector bytes;
    };

    struct DecodedWorkItem
    {
        std::uint64_t sequence = 0U;
        stgs::FrameParseResult result;
    };

    /**
     * @brief Conserve la première exception asynchrone produite par un worker.
     *
     * Les threads ne laissent jamais une exception franchir leur point d'entrée, ce qui appellerait
     * `std::terminate`. Ils enregistrent la première erreur ici, ferment les files et le thread
     * principal la relance après les `join()` pour conserver un chemin d'erreur déterministe.
     */
    class AsyncFailure
    {
    public:
        void capture(std::exception_ptr error)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (error_ == nullptr)
            {
                error_ = std::move(error);
            }
        }

        void rethrowIfPresent() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (error_ != nullptr)
            {
                std::rethrow_exception(error_);
            }
        }

    private:
        mutable std::mutex mutex_;
        std::exception_ptr error_;
    };

    void printUsage()
    {
        std::cout << R"(Space Telemetry Ground Station

Usage:
  stgs_ground_station --udp --port 9000 [options]
  stgs_ground_station --tcp --port 9000 [options]
  stgs_ground_station --tcp --auto-port 9000:9010 [options]
  stgs_ground_station --replay frames.stgf [options]

Input:
  --udp                           Listen for one telemetry frame per UDP datagram.
  --tcp                           Listen for telemetry frames over a TCP byte stream.
  --bind <ipv4>                   Bind address, default 0.0.0.0.
  --port <port>                   Listening port, default 9000.
  --auto-port <a:b>               TCP only: select first locally bindable port in a bounded range.
  --replay <file>                 Replay frames from an STGF capture file.
  --replay-rate <fps>             Replay speed, 0 = as fast as possible.

Pipeline / output:
  --decoder-threads <n>           Parallel decoder workers, default 2.
  --queue-capacity <n>            Backpressure capacity per pipeline queue, default 1024.
  --output <file>                 Output file, default telemetry.csv.
  --output-format <csv|json>      Otherwise inferred from extension.
  --signal-filter <mode>          none, moving-average, sine-projection (default).
  --filter-window <odd-n>         Moving-average FIR window, default 5.

Health monitoring (demo thresholds, configurable):
  --disable-degraded
  --degraded-window <n>           Default 100 samples.
  --degraded-min-samples <n>      Default 20 samples.
  --degraded-rejection-rate <p>   NOMINAL -> DEGRADED, default 0.10.
  --degraded-recovery-rate <p>    DEGRADED -> NOMINAL, default 0.03.
  --degraded-critical-count <n>   Critical count to degrade, default 8.
  --recovery-critical-count <n>   Critical count allowed to recover, default 3.

Diagnostics:
  --log <file>                    Optional log file.
  --log-level <level>             trace, debug, info, warn, error.
  --verbose                       Log each decoded/rejected frame at debug level.
  --no-color                      Disable ANSI colors.
  --help                          Show this help.

Related command:
  stgs_port_check --host 127.0.0.1 --ports 9000:9010
)";
    }

    std::string requireValue(int &index, int argc, char **argv)
    {
        if (index + 1 >= argc)
        {
            throw std::runtime_error(std::string("missing value after ") + argv[index]);
        }
        ++index;
        return argv[index];
    }

    void selectTransport(Options &options, stgs::Transport transport)
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

    stgs::SignalFilterMode parseSignalFilter(const std::string &value)
    {
        if (value == "none")
        {
            return stgs::SignalFilterMode::None;
        }
        if (value == "moving-average")
        {
            return stgs::SignalFilterMode::MovingAverage;
        }
        if (value == "sine-projection")
        {
            return stgs::SignalFilterMode::SineProjection;
        }
        throw std::runtime_error("--signal-filter must be none, moving-average or sine-projection");
    }

    OutputFormat resolveOutputFormat(OutputFormat requested, const std::filesystem::path &output)
    {
        if (requested != OutputFormat::Auto)
        {
            return requested;
        }
        return output.extension() == ".json" ? OutputFormat::Json : OutputFormat::Csv;
    }

    /**
     * @brief Parse et valide l'ensemble de la configuration de la station avant tout effet de bord.
     *
     * Les exclusions de modes (réseau/replay, UDP/TCP, port explicite/auto-port), les bornes des
     * files/workers et la cohérence de l'hystérésis sont contrôlées ici. Cette validation en amont
     * évite de démarrer threads, fichiers ou sockets avec une configuration qui échouera plus tard.
     *
     * @return Options cohérentes prêtes à être appliquées par main().
     * @throws std::runtime_error Pour toute option inconnue, valeur invalide ou combinaison interdite.
     */
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
                selectTransport(options, stgs::Transport::Udp);
            }
            else if (arg == "--tcp")
            {
                selectTransport(options, stgs::Transport::Tcp);
            }
            else if (arg == "--bind")
            {
                options.bindAddress = requireValue(i, argc, argv);
            }
            else if (arg == "--port")
            {
                options.port = stgs::parseUnsigned<std::uint16_t>(
                    requireValue(i, argc, argv), "--port", stgs::MinimumNetworkPort, stgs::MaximumNetworkPort);
                portExplicit = true;
            }
            else if (arg == "--auto-port")
            {
                options.autoPortRange = stgs::parsePortRange(requireValue(i, argc, argv), "--auto-port");
            }
            else if (arg == "--replay")
            {
                options.replayFile = requireValue(i, argc, argv);
            }
            else if (arg == "--replay-rate")
            {
                options.replayRate = stgs::parseFiniteDouble(requireValue(i, argc, argv), "--replay-rate");
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
                const auto level = stgs::parseLogLevel(requireValue(i, argc, argv));
                if (!level.has_value())
                {
                    throw std::runtime_error("unknown log level");
                }
                options.logLevel = *level;
            }
            else if (arg == "--verbose")
            {
                options.verbose = true;
                options.logLevel = stgs::LogLevel::Debug;
            }
            else if (arg == "--decoder-threads")
            {
                options.decoderThreads = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--decoder-threads", 1U, MaximumDecoderThreads);
            }
            else if (arg == "--queue-capacity")
            {
                options.queueCapacity = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--queue-capacity", 1U, MaximumQueueCapacity);
            }
            else if (arg == "--signal-filter")
            {
                options.signalFilter = parseSignalFilter(requireValue(i, argc, argv));
            }
            else if (arg == "--filter-window")
            {
                options.movingAverageWindow = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--filter-window", 1U, MaximumFilterWindow);
            }
            else if (arg == "--disable-degraded")
            {
                options.healthConfig.enabled = false;
            }
            else if (arg == "--degraded-window")
            {
                options.healthConfig.windowSize = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--degraded-window", 1U, MaximumHealthWindowSamples);
            }
            else if (arg == "--degraded-min-samples")
            {
                options.healthConfig.minSamples = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--degraded-min-samples", 1U, MaximumHealthWindowSamples);
            }
            else if (arg == "--degraded-rejection-rate")
            {
                options.healthConfig.degradedRejectionRate =
                    stgs::parseProbability(requireValue(i, argc, argv), "--degraded-rejection-rate");
            }
            else if (arg == "--degraded-recovery-rate")
            {
                options.healthConfig.recoveryRejectionRate =
                    stgs::parseProbability(requireValue(i, argc, argv), "--degraded-recovery-rate");
            }
            else if (arg == "--degraded-critical-count")
            {
                options.healthConfig.criticalFramesForDegraded = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--degraded-critical-count", 1U, MaximumHealthWindowSamples);
            }
            else if (arg == "--recovery-critical-count")
            {
                options.healthConfig.criticalFramesForRecovery = stgs::parseUnsigned<std::size_t>(
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
            if (!options.transport.has_value() || *options.transport != stgs::Transport::Tcp)
            {
                throw std::runtime_error("--auto-port requires --tcp");
            }
            if (portExplicit)
            {
                throw std::runtime_error("choose either --port or --auto-port, not both");
            }
            if (options.autoPortRange->count() > stgs::MaxDiagnosticPortCount)
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

    std::string csvEscape(const std::string &value)
    {
        std::string out = "\"";
        for (const char ch : value)
        {
            if (ch == '"')
            {
                out += "\"\"";
            }
            else
            {
                out += ch;
            }
        }
        out += '"';
        return out;
    }

    std::string jsonEscape(const std::string &value)
    {
        std::ostringstream out;
        for (const char raw : value)
        {
            const auto ch = static_cast<unsigned char>(raw);
            switch (ch)
            {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < JsonControlUpperExclusive)
                {
                    out << "\\u" << std::hex << std::setw(JsonUnicodeEscapeHexWidth) << std::setfill('0')
                        << static_cast<unsigned int>(ch) << std::dec;
                }
                else
                {
                    out << static_cast<char>(ch);
                }
                break;
            }
        }
        return out.str();
    }

    void writeCsvHeader(std::ofstream &out)
    {
        out << "timestamp_ms,satellite_id,temperature_c,battery_percent,status,payload_len,payload_hex\n";
    }

    void writeFrameCsv(std::ofstream &out, const stgs::TelemetryFrame &frame)
    {
        out << frame.timestampMs << ','
            << frame.satelliteId << ','
            << std::setprecision(std::numeric_limits<float>::max_digits10) << frame.temperatureC << ','
            << static_cast<unsigned int>(frame.batteryPercent) << ','
            << stgs::statusToString(frame.status) << ','
            << frame.payload.size() << ','
            << csvEscape(stgs::payloadToHex(frame.payload, frame.payload.size())) << '\n';
    }

    void writeJsonHeader(std::ofstream &out) { out << "[\n"; }

    void writeFrameJson(std::ofstream &out, const stgs::TelemetryFrame &frame, bool first)
    {
        if (!first)
        {
            out << ",\n";
        }
        out << "  {"
            << "\"timestamp_ms\":" << frame.timestampMs << ','
            << "\"satellite_id\":" << frame.satelliteId << ','
            << "\"temperature_c\":"
            << std::setprecision(std::numeric_limits<float>::max_digits10) << frame.temperatureC << ','
            << "\"battery_percent\":" << static_cast<unsigned int>(frame.batteryPercent) << ','
            << "\"status\":\"" << jsonEscape(stgs::statusToString(frame.status)) << "\","
            << "\"payload_len\":" << frame.payload.size() << ','
            << "\"payload_hex\":\"" << jsonEscape(stgs::payloadToHex(frame.payload, frame.payload.size())) << "\""
            << '}';
    }

    void writeJsonFooter(std::ofstream &out) { out << "\n]\n"; }

    void logTransition(stgs::Logger &logger, const stgs::StationStateTransition &transition)
    {
        std::ostringstream message;
        message << "station state changed: " << stgs::stationStateToString(transition.from)
                << " -> " << stgs::stationStateToString(transition.to)
                << ", samples=" << transition.snapshot.samples
                << ", rejected=" << transition.snapshot.rejected
                << ", rejection_rate=" << transition.snapshot.rejectionRate
                << ", critical=" << transition.snapshot.critical
                << ", reason=" << transition.reason;
        if (transition.to == stgs::StationState::Degraded)
        {
            logger.warning(message.str());
        }
        else
        {
            logger.info(message.str());
        }
    }

    /**
     * @brief Interprète un payload STGA et déclenche le rendu opérateur approprié.
     *
     * Un payload historique non-STGA est ignoré silencieusement. Une enveloppe STGA malformée est
     * signalée mais ne transforme pas la TelemetryFrame externe en rejet : son CRC et son framing
     * étaient valides, seule la couche applicative n'a pas pu être interprétée.
     */
    void processApplicationPayload(const Options &options,
                                   const stgs::TelemetryFrame &frame,
                                   stgs::TerminalUi &ui,
                                   stgs::Logger &logger,
                                   std::uint64_t &applicationErrors)
    {
        auto parsed = stgs::decodeApplicationPayload(frame.payload);
        if (std::holds_alternative<std::monostate>(parsed))
        {
            return;
        }
        if (const auto *message = std::get_if<stgs::TextMessagePayload>(&parsed))
        {
            ui.receivedMessage(frame.satelliteId, message->sequence, message->text);
            return;
        }
        if (const auto *signal = std::get_if<stgs::SignalBlockPayload>(&parsed))
        {
            std::vector<float> filtered;
            switch (options.signalFilter)
            {
            case stgs::SignalFilterMode::None:
                filtered = signal->samples;
                break;
            case stgs::SignalFilterMode::MovingAverage:
                filtered = stgs::movingAverageFilter(signal->samples, options.movingAverageWindow);
                break;
            case stgs::SignalFilterMode::SineProjection:
                filtered = stgs::sineProjectionFilter(signal->samples,
                                                      signal->sampleRateHz,
                                                      signal->frequencyHz,
                                                      signal->startSampleIndex);
                break;
            }
            const auto metrics = stgs::computeSignalMetrics(signal->samples,
                                                            filtered,
                                                            signal->sampleRateHz,
                                                            signal->frequencyHz,
                                                            signal->startSampleIndex);
            ui.receivedSignal(frame.satelliteId,
                              signal->sampleRateHz,
                              signal->frequencyHz,
                              signal->nominalAmplitude,
                              signal->samples,
                              filtered,
                              metrics,
                              options.signalFilter);
            return;
        }

        const auto &error = std::get<stgs::ApplicationPayloadError>(parsed);
        ++applicationErrors;
        logger.warning(std::string("application payload rejected: ") +
                       stgs::applicationPayloadErrorToString(error.code) + " - " + error.message);
    }

} // namespace

int main(int argc, char **argv)
{
    try
    {
        auto options = parseArgs(argc, argv);
        const auto outputFormat = resolveOutputFormat(options.outputFormat, options.outputFile);
        stgs::TerminalUi ui(options.color);

        if (options.autoPortRange.has_value())
        {
            ui.section("LOCAL PORT SELECTION");
            std::vector<stgs::LocalPortAvailability> report;
            const auto selected = stgs::findFirstAvailableLocalPort(
                options.bindAddress, *options.autoPortRange, stgs::PortTransport::Tcp, &report);
            for (const auto &entry : report)
            {
                ui.keyValue("port " + std::to_string(entry.port),
                            std::string(entry.available ? "AVAILABLE" : "BUSY/ERROR") + " / " + entry.detail);
            }
            if (!selected.has_value())
            {
                throw std::runtime_error("no bindable port found in --auto-port range");
            }
            options.port = *selected;
            ui.keyValue("selected", std::to_string(options.port));
        }

        stgs::Logger logger(options.logFile, options.logLevel);
        stgs::StationHealthMonitor health(options.healthConfig);
        std::atomic_bool running{true};
        PosixSignalStopController signalStopController(running);

        ui.section("GROUND STATION STARTUP");
        if (options.replayFile.has_value())
        {
            ui.keyValue("input", "replay " + options.replayFile->string());
        }
        else
        {
            ui.keyValue("input", stgs::transportToString(*options.transport));
            ui.keyValue("listen", options.bindAddress + ":" + std::to_string(options.port));
        }
        ui.keyValue("decoder workers", std::to_string(options.decoderThreads));
        ui.keyValue("queue capacity", std::to_string(options.queueCapacity));
        ui.keyValue("output", options.outputFile.string());
        ui.keyValue("signal filter", stgs::signalFilterModeToString(options.signalFilter));
        ui.keyValue("health critical hysteresis",
                    std::to_string(options.healthConfig.criticalFramesForDegraded) + " -> " +
                    std::to_string(options.healthConfig.criticalFramesForRecovery));

        ui.section("PIPELINE");
        ui.keyValue("stage 1", "network/replay -> bounded raw queue");
        ui.keyValue("stage 2", "parallel FrameCodec + CRC validation");
        ui.keyValue("stage 3", "sequence reorder -> deterministic health/export");
        ui.keyValue("stage 4", "STGA message/signal decode -> terminal rendering");

        stgs::BlockingQueue<RawWorkItem> rawQueue(options.queueCapacity);
        stgs::BlockingQueue<DecodedWorkItem> decodedQueue(options.queueCapacity);
        std::atomic<std::uint64_t> nextSequence{0U};
        std::atomic<std::uint64_t> received{0U};
        std::atomic<std::uint64_t> decoded{0U};
        std::atomic<std::uint64_t> rejected{0U};
        std::atomic<std::uint64_t> written{0U};
        std::atomic<std::uint64_t> applicationErrors{0U};
        AsyncFailure asyncFailure;

        /**
         * Observabilité périodique du pipeline :
         * un thread léger se réveille au plus une fois par seconde et ne rend une ligne que si les
         * compteurs ont évolué. `condition_variable_any` avec stop_token permet un arrêt immédiat
         * du moniteur sans attendre la fin de l'intervalle. Les tailles de files sont lues via leur
         * API verrouillée ; aucun état interne n'est exposé sans synchronisation.
         */
        std::condition_variable_any progressWakeup;
        std::mutex progressMutex;
        std::jthread progressThread([&](std::stop_token stopToken)
        {
            std::uint64_t previousReceived = 0U;
            std::uint64_t previousDecoded = 0U;
            std::uint64_t previousRejected = 0U;
            std::uint64_t previousWritten = 0U;
            std::unique_lock<std::mutex> lock(progressMutex);

            while (!stopToken.stop_requested())
            {
                (void)progressWakeup.wait_for(
                    lock, stopToken, PipelineProgressInterval, [] { return false; });
                if (stopToken.stop_requested())
                {
                    break;
                }

                const auto currentReceived = received.load();
                const auto currentDecoded = decoded.load();
                const auto currentRejected = rejected.load();
                const auto currentWritten = written.load();
                if (currentReceived == previousReceived &&
                    currentDecoded == previousDecoded &&
                    currentRejected == previousRejected &&
                    currentWritten == previousWritten)
                {
                    continue;
                }

                // Ne conserver aucun verrou du moniteur pendant le rendu terminal ou le verrouillage
                // interne des files : cette séparation évite tout ordre de verrous implicite.
                lock.unlock();
                std::ostringstream detail;
                detail << "rx=" << currentReceived
                       << " decoded=" << currentDecoded
                       << " rejected=" << currentRejected
                       << " written=" << currentWritten
                       << " queues=" << rawQueue.size() << '/' << rawQueue.capacity()
                       << " -> " << decodedQueue.size() << '/' << decodedQueue.capacity();
                ui.statusLine(stgs::TerminalStatus::Info, "pipeline", detail.str());
                lock.lock();

                previousReceived = currentReceived;
                previousDecoded = currentDecoded;
                previousRejected = currentRejected;
                previousWritten = currentWritten;
            }
        });

        std::ofstream out(options.outputFile, std::ios::out | std::ios::trunc);
        if (!out)
        {
            throw std::runtime_error("failed to open output file: " + options.outputFile.string());
        }
        if (outputFormat == OutputFormat::Json)
        {
            writeJsonHeader(out);
        }
        else
        {
            writeCsvHeader(out);
        }

        const auto failPipeline = [&](std::exception_ptr error)
        {
            asyncFailure.capture(std::move(error));
            running.store(false);
            rawQueue.close();
            decodedQueue.close();
        };

        /**
         * Writer/ordering stage:
         * les décodeurs terminent potentiellement dans un ordre différent. Chaque résultat porte
         * donc le numéro attribué avant leur file de travail. Une map temporaire retient les trous et
         * ne traite que `expectedSequence`. Santé, messages, signaux et export restent ainsi
         * strictement reproductibles avec 1, 4 ou 32 workers.
         */
        std::jthread writerThread([&]
        {
            try
            {
                bool firstJsonFrame = true;
                std::uint64_t expectedSequence = 0U;
                std::map<std::uint64_t, DecodedWorkItem> pending;
                std::uint64_t localApplicationErrors = 0U;

                while (auto incoming = decodedQueue.pop())
                {
                    const auto [iterator, inserted] = pending.emplace(incoming->sequence, std::move(*incoming));
                    if (!inserted)
                    {
                        throw std::runtime_error("duplicate pipeline sequence detected");
                    }
                    (void)iterator;

                    while (true)
                    {
                        auto current = pending.find(expectedSequence);
                        if (current == pending.end())
                        {
                            break;
                        }

                        if (auto *frame = std::get_if<stgs::TelemetryFrame>(&current->second.result))
                        {
                            ++decoded;
                            if (auto transition = health.recordDecoded(*frame); transition.has_value())
                            {
                                logTransition(logger, *transition);
                            }
                            processApplicationPayload(options, *frame, ui, logger, localApplicationErrors);
                            if (outputFormat == OutputFormat::Json)
                            {
                                writeFrameJson(out, *frame, firstJsonFrame);
                                firstJsonFrame = false;
                            }
                            else
                            {
                                writeFrameCsv(out, *frame);
                            }
                            if (!out)
                            {
                                throw std::runtime_error("failed while writing telemetry output");
                            }
                            ++written;
                            if (options.verbose)
                            {
                                logger.debug("decoded seq=" + std::to_string(expectedSequence) + " " + stgs::toString(*frame));
                            }
                        }
                        else
                        {
                            ++rejected;
                            const auto &error = std::get<stgs::FrameError>(current->second.result);
                            if (auto transition = health.recordRejected(); transition.has_value())
                            {
                                logTransition(logger, *transition);
                            }
                            logger.warning("rejected seq=" + std::to_string(expectedSequence) + ": " +
                                           stgs::errorCodeToString(error.code) + " - " + error.message);
                        }

                        pending.erase(current);
                        ++expectedSequence;
                    }
                }

                if (!pending.empty())
                {
                    throw std::runtime_error("pipeline closed with a sequence gap in decoded results");
                }
                applicationErrors.store(localApplicationErrors);
                if (outputFormat == OutputFormat::Json)
                {
                    writeJsonFooter(out);
                }
                out.flush();
                if (!out)
                {
                    throw std::runtime_error("failed while flushing telemetry output");
                }
            }
            catch (...)
            {
                failPipeline(std::current_exception());
            }
        });

        std::vector<std::jthread> decoderThreads;
        decoderThreads.reserve(options.decoderThreads);
        for (std::size_t workerIndex = 0U; workerIndex < options.decoderThreads; ++workerIndex)
        {
            decoderThreads.emplace_back([&, workerIndex]
            {
                try
                {
                    while (auto work = rawQueue.pop())
                    {
                        DecodedWorkItem result;
                        result.sequence = work->sequence;
                        result.result = stgs::decodeFrame(work->bytes);
                        if (!decodedQueue.push(std::move(result)))
                        {
                            return;
                        }
                        if (options.verbose)
                        {
                            logger.trace("decoder worker=" + std::to_string(workerIndex) +
                                         " completed seq=" + std::to_string(work->sequence));
                        }
                    }
                }
                catch (...)
                {
                    failPipeline(std::current_exception());
                }
            });
        }

        const auto submit = [&](stgs::ByteVector frameBytes)
        {
            RawWorkItem work;
            work.sequence = nextSequence.fetch_add(1U);
            work.bytes = std::move(frameBytes);
            if (!rawQueue.push(std::move(work)))
            {
                running.store(false);
                return;
            }
            ++received;
        };

        ui.section("RECEPTION");
        if (options.replayFile.has_value())
        {
            stgs::FrameFileReader reader(*options.replayFile);
            logger.info("replay started from " + options.replayFile->string());
            const auto delay = options.replayRate > 0.0
                                   ? std::chrono::duration<double>(1.0 / options.replayRate)
                                   : std::chrono::duration<double>(0.0);
            auto nextTick = std::chrono::steady_clock::now();
            while (running.load())
            {
                auto frame = reader.readNext();
                if (!frame.has_value())
                {
                    break;
                }
                submit(std::move(*frame));
                if (delay.count() > 0.0)
                {
                    nextTick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
                    std::this_thread::sleep_until(nextTick);
                }
            }
            logger.info("replay completed");
            running.store(false);
        }
        else
        {
            stgs::NetworkConfig config;
            config.transport = *options.transport;
            config.bindAddress = options.bindAddress;
            config.port = options.port;
            stgs::NetworkServer server(config, logger);
            server.run(submit, running);
        }

        rawQueue.close();
        for (auto &thread : decoderThreads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        decodedQueue.close();
        if (writerThread.joinable())
        {
            writerThread.join();
        }
        progressThread.request_stop();
        progressThread.join();
        asyncFailure.rethrowIfPresent();

        const auto snapshot = health.snapshot();
        ui.section("GROUND STATION SUMMARY");
        ui.keyValue("received", std::to_string(received.load()));
        ui.keyValue("decoded", std::to_string(decoded.load()));
        ui.keyValue("rejected", std::to_string(rejected.load()));
        ui.keyValue("written", std::to_string(written.load()));
        ui.keyValue("application errors", std::to_string(applicationErrors.load()));
        ui.keyValue("station state", stgs::stationStateToString(snapshot.state));
        ui.keyValue("rejection rate", std::to_string(snapshot.rejectionRate));
        ui.keyValue("output", options.outputFile.string());

        std::ostringstream summary;
        summary << "summary received=" << received.load()
                << " decoded=" << decoded.load()
                << " rejected=" << rejected.load()
                << " written=" << written.load()
                << " application_errors=" << applicationErrors.load()
                << " station_state=" << stgs::stationStateToString(snapshot.state)
                << " rejection_rate=" << snapshot.rejectionRate
                << " output=" << options.outputFile.string();
        logger.info(summary.str());
        signalStopController.finish();
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        std::cerr << "run with --help for usage\n";
        return 1;
    }
}
