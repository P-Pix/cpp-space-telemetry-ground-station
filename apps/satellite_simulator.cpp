/**
 * @file satellite_simulator.cpp
 * @brief Génère des télémétries STGS aléatoires, des messages texte ou des blocs de signal.
 *
 * Le simulateur sait envoyer en UDP/TCP, produire un replay STGF, rechercher un listener TCP sur
 * une plage loopback bornée et injecter deux familles de perturbations distinctes : bruit analogique
 * simulé dans les échantillons et corruption binaire après calcul du CRC.
 */

#include "stgs/ApplicationPayload.hpp"
#include "stgs/CliParsing.hpp"
#include "stgs/FrameCodec.hpp"
#include "stgs/NetworkServer.hpp"
#include "stgs/PortDiagnostics.hpp"
#include "stgs/Replay.hpp"
#include "stgs/TerminalUi.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <numbers>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace
{

    // Valeurs de démonstration nommées : elles peuvent être changées sans rechercher des littéraux
    // dispersés dans le parsing, le help et la génération. Les maxima sont des garde-fous CLI.
    inline constexpr std::size_t DefaultGeneratedFrameCount = 1000U;
    inline constexpr std::size_t DefaultRandomPayloadSize = 32U;
    inline constexpr std::size_t DefaultSignalFrameCount = 5U;
    inline constexpr std::size_t DefaultMessageFrameCount = 1U;
    inline constexpr std::size_t MaximumGeneratedFrameCount = 10'000'000U;
    inline constexpr std::uint16_t DefaultSatelliteId = 42U;
    inline constexpr std::uint16_t DefaultSignalSampleRateHz = 200U;
    inline constexpr float DefaultSignalFrequencyHz = 5.0F;
    inline constexpr float DefaultSignalAmplitude = 1.0F;
    inline constexpr std::size_t DefaultSignalSampleCount = 256U;
    inline constexpr double DefaultTransmissionRateFps = 100.0;

    enum class PayloadMode
    {
        RandomBytes,
        TextMessage,
        Signal
    };

    struct Options
    {
        std::optional<stgs::Transport> transport;
        std::string host = "127.0.0.1";
        std::uint16_t port = stgs::DefaultTelemetryPort;
        std::optional<stgs::PortRange> discoverPorts;
        std::chrono::milliseconds discoveryTimeout{stgs::DefaultPortProbeTimeout};
        std::size_t count = DefaultGeneratedFrameCount;
        bool countExplicit = false;
        double rate = DefaultTransmissionRateFps;
        std::uint16_t satelliteId = DefaultSatelliteId;
        std::size_t payloadSize = DefaultRandomPayloadSize;
        double lossProbability = 0.0;
        double corruptProbability = 0.0;
        std::uint32_t seed = std::random_device{}();
        std::optional<std::filesystem::path> outputFile;
        PayloadMode payloadMode = PayloadMode::RandomBytes;
        std::string message;
        std::uint16_t sampleRateHz = DefaultSignalSampleRateHz;
        float signalFrequencyHz = DefaultSignalFrequencyHz;
        float signalAmplitude = DefaultSignalAmplitude;
        std::size_t signalSamples = DefaultSignalSampleCount;
        float noiseStddev = 0.0F;
        bool color = true;
    };

    // Paramètres de démonstration explicitement nommés : ils ne proviennent pas d'un satellite réel.
    inline constexpr float SimulationTemperatureMeanC = 22.0F;
    inline constexpr float SimulationTemperatureStddevC = 4.0F;
    // Distribution de démonstration : 90 % NOMINAL, 7 % WARNING, 2 % CRITICAL, 1 % SAFE_MODE.
    // Elle sert à exercer la logique de santé ; elle ne modélise aucune mission réelle.
    inline constexpr std::array<double, 4U> SimulationStatusWeights{90.0, 7.0, 2.0, 1.0};
    inline constexpr std::uint8_t CorruptionXorMask = 0x5AU;
    // 101 valeurs permettent une rampe artificielle 100,99,...,0 puis reprise à 100.
    inline constexpr std::size_t SimulationBatteryCycleLength =
        static_cast<std::size_t>(stgs::MaxBatteryPercent) + 1U;
    inline constexpr double TwoPi = 2.0 * std::numbers::pi_v<double>;

    class SocketFd
    {
    public:
        explicit SocketFd(int fd = -1) noexcept : fd_(fd) {}
        ~SocketFd()
        {
            if (fd_ >= 0)
            {
                ::close(fd_);
            }
        }
        SocketFd(const SocketFd &) = delete;
        SocketFd &operator=(const SocketFd &) = delete;
        SocketFd(SocketFd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
        SocketFd &operator=(SocketFd &&other) noexcept
        {
            if (this != &other)
            {
                if (fd_ >= 0)
                {
                    ::close(fd_);
                }
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }
        [[nodiscard]] int get() const noexcept { return fd_; }

    private:
        int fd_ = -1;
    };

    void printUsage()
    {
        std::cout << R"(Satellite Telemetry Simulator

Usage:
  stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 [options]
  stgs_satellite_simulator --tcp --host 127.0.0.1 --discover-ports 9000:9010 [options]
  stgs_satellite_simulator --udp --host 127.0.0.1 --port 9000 [options]
  stgs_satellite_simulator --output-file frames.stgf [options]

Payload modes (mutually exclusive):
  default                         Random binary payload.
  --message <text>                Send typed STGA text messages.
  --signal                        Send typed STGA sine-signal blocks.

Network / replay:
  --udp                           Send one STGS frame per UDP datagram.
  --tcp                           Send frames over a TCP byte stream.
  --host <ipv4>                   Destination host, default 127.0.0.1.
  --port <port>                   Destination port, default 9000.
  --discover-ports <a:b>          TCP loopback only: report ports then use first OPEN listener.
  --discovery-timeout-ms <ms>     Timeout per local TCP probe, default 200.
  --output-file <file>            Also write transmitted wire frames to an STGF replay.

Traffic:
  --count <n>                     Number of frames. Defaults: random=1000, message=1, signal=5.
  --rate <fps>                    Frames/second, 0 = as fast as possible. Default 100.
  --satellite <id>                Satellite ID (0..65535), default 42.
  --payload-size <bytes>          Random mode only, default 32, max 4096.
  --loss <p>                      Simulated drop probability [0,1].
  --corrupt <p>                   Corrupt bytes AFTER CRC with probability [0,1].
  --seed <n>                      Deterministic 32-bit RNG seed.

Signal mode:
  --sample-rate <hz>              Sampling rate, default 200 Hz.
  --signal-frequency <hz>         Sine frequency, default 5 Hz, strictly below Nyquist.
  --signal-amplitude <value>      Nominal amplitude, default 1.0.
  --signal-samples <n>            Samples per STGA block, default 256.
  --noise-stddev <value>          Gaussian noise sigma added BEFORE CRC, default 0.

Display:
  --no-color                      Disable ANSI colors.
  --help                          Show this help.
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

    float parseFiniteFloat(const std::string &value, const std::string &name)
    {
        const double parsed = stgs::parseFiniteDouble(value, name);
        if (parsed < -static_cast<double>(std::numeric_limits<float>::max()) ||
            parsed > static_cast<double>(std::numeric_limits<float>::max()))
        {
            throw std::runtime_error(name + " exceeds float range");
        }
        return static_cast<float>(parsed);
    }

    /**
     * @brief Parse la CLI et effectue les validations qui doivent précéder toute transmission.
     *
     * La fonction vérifie les modes exclusifs, les plages numériques, Nyquist pour le signal, la
     * taille réellement encodable d'un payload STGA et la capacité du compteur absolu d'échantillons.
     * Le simulateur ne commence donc pas une longue émission pour découvrir une incohérence à mi-run.
     *
     * @return Configuration entièrement validée du générateur.
     * @throws std::runtime_error Si la syntaxe, les bornes ou les combinaisons d'options sont invalides.
     */
    Options parseArgs(int argc, char **argv)
    {
        Options options;
        bool portExplicit = false;
        bool payloadSizeExplicit = false;
        bool messageSelected = false;
        bool signalSelected = false;

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
            else if (arg == "--host")
            {
                options.host = requireValue(i, argc, argv);
            }
            else if (arg == "--port")
            {
                options.port = stgs::parseUnsigned<std::uint16_t>(
                    requireValue(i, argc, argv), "--port", stgs::MinimumNetworkPort, stgs::MaximumNetworkPort);
                portExplicit = true;
            }
            else if (arg == "--discover-ports")
            {
                options.discoverPorts = stgs::parsePortRange(requireValue(i, argc, argv), "--discover-ports");
            }
            else if (arg == "--discovery-timeout-ms")
            {
                const auto timeout = stgs::parseUnsigned<unsigned int>(
                    requireValue(i, argc, argv), "--discovery-timeout-ms", 1U, static_cast<unsigned int>(stgs::MaximumPortProbeTimeout.count()));
                options.discoveryTimeout = std::chrono::milliseconds(timeout);
            }
            else if (arg == "--count")
            {
                options.count = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--count", 1U, MaximumGeneratedFrameCount);
                options.countExplicit = true;
            }
            else if (arg == "--rate")
            {
                options.rate = stgs::parseFiniteDouble(requireValue(i, argc, argv), "--rate");
                if (options.rate < 0.0)
                {
                    throw std::runtime_error("--rate must be >= 0");
                }
            }
            else if (arg == "--satellite")
            {
                options.satelliteId = stgs::parseUnsigned<std::uint16_t>(
                    requireValue(i, argc, argv), "--satellite");
            }
            else if (arg == "--payload-size")
            {
                options.payloadSize = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--payload-size", 0U, stgs::MaxPayloadSize);
                payloadSizeExplicit = true;
            }
            else if (arg == "--loss")
            {
                options.lossProbability = stgs::parseProbability(requireValue(i, argc, argv), "--loss");
            }
            else if (arg == "--corrupt")
            {
                options.corruptProbability = stgs::parseProbability(requireValue(i, argc, argv), "--corrupt");
            }
            else if (arg == "--seed")
            {
                options.seed = stgs::parseUnsigned<std::uint32_t>(requireValue(i, argc, argv), "--seed");
            }
            else if (arg == "--output-file")
            {
                options.outputFile = requireValue(i, argc, argv);
            }
            else if (arg == "--message")
            {
                options.payloadMode = PayloadMode::TextMessage;
                options.message = requireValue(i, argc, argv);
                messageSelected = true;
            }
            else if (arg == "--signal")
            {
                options.payloadMode = PayloadMode::Signal;
                signalSelected = true;
            }
            else if (arg == "--sample-rate")
            {
                options.sampleRateHz = stgs::parseUnsigned<std::uint16_t>(
                    requireValue(i, argc, argv), "--sample-rate", 1U,
                    std::numeric_limits<std::uint16_t>::max());
            }
            else if (arg == "--signal-frequency")
            {
                options.signalFrequencyHz = parseFiniteFloat(requireValue(i, argc, argv), "--signal-frequency");
            }
            else if (arg == "--signal-amplitude")
            {
                options.signalAmplitude = parseFiniteFloat(requireValue(i, argc, argv), "--signal-amplitude");
            }
            else if (arg == "--signal-samples")
            {
                options.signalSamples = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--signal-samples", 1U,
                    stgs::MaxSignalSamplesPerPayload);
            }
            else if (arg == "--noise-stddev")
            {
                options.noiseStddev = parseFiniteFloat(requireValue(i, argc, argv), "--noise-stddev");
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

        if (messageSelected && signalSelected)
        {
            throw std::runtime_error("--message and --signal are mutually exclusive payload modes");
        }
        if (payloadSizeExplicit && options.payloadMode != PayloadMode::RandomBytes)
        {
            throw std::runtime_error("--payload-size applies only to the default random payload mode");
        }
        if (!options.transport.has_value() && !options.outputFile.has_value())
        {
            throw std::runtime_error("choose --udp, --tcp, and/or --output-file");
        }
        if (options.discoverPorts.has_value())
        {
            if (!options.transport.has_value() || *options.transport != stgs::Transport::Tcp)
            {
                throw std::runtime_error("--discover-ports requires --tcp");
            }
            if (portExplicit)
            {
                throw std::runtime_error("choose either --port or --discover-ports, not both");
            }
            if (!stgs::isLoopbackIpv4(options.host))
            {
                throw std::runtime_error("--discover-ports is intentionally restricted to IPv4 loopback");
            }
            if (options.discoverPorts->count() > stgs::MaxDiagnosticPortCount)
            {
                throw std::runtime_error("--discover-ports exceeds MaxDiagnosticPortCount");
            }
        }
        if (options.signalAmplitude < 0.0F)
        {
            throw std::runtime_error("--signal-amplitude must be >= 0");
        }
        if (options.noiseStddev < 0.0F)
        {
            throw std::runtime_error("--noise-stddev must be >= 0");
        }
        if (options.signalFrequencyHz <= 0.0F ||
            options.signalFrequencyHz >= static_cast<float>(options.sampleRateHz) / 2.0F)
        {
            throw std::runtime_error("--signal-frequency must be > 0 and strictly below the Nyquist frequency");
        }

        if (!options.countExplicit)
        {
            if (options.payloadMode == PayloadMode::TextMessage)
            {
                options.count = DefaultMessageFrameCount;
            }
            else if (options.payloadMode == PayloadMode::Signal)
            {
                options.count = DefaultSignalFrameCount;
            }
        }

        // Encode un bloc témoin pour appliquer exactement la limite réelle du format STGA.
        if (options.payloadMode == PayloadMode::Signal)
        {
            stgs::SignalBlockPayload probe;
            probe.sampleRateHz = options.sampleRateHz;
            probe.frequencyHz = options.signalFrequencyHz;
            probe.nominalAmplitude = options.signalAmplitude;
            probe.samples.assign(options.signalSamples, 0.0F);
            (void)stgs::encodeSignalBlockPayload(probe);

            const std::uint64_t lastBlockIndex = static_cast<std::uint64_t>(options.count - 1U);
            const std::uint64_t finalBlockStart =
                lastBlockIndex * static_cast<std::uint64_t>(options.signalSamples);
            if (finalBlockStart > std::numeric_limits<std::uint32_t>::max())
            {
                throw std::runtime_error(
                    "signal run exceeds STGA v1 32-bit startSampleIndex; reduce --count or --signal-samples");
            }
        }
        if (options.payloadMode == PayloadMode::TextMessage)
        {
            (void)stgs::encodeTextMessagePayload(stgs::TextMessagePayload{0U, options.message});
        }
        return options;
    }

    std::uint64_t nowMs()
    {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }

    std::string normalizeIpv4Host(const std::string &host)
    {
        return host == "localhost" ? "127.0.0.1" : host;
    }

    SocketFd createSocket(stgs::Transport transport)
    {
        const int type = transport == stgs::Transport::Udp ? SOCK_DGRAM : SOCK_STREAM;
        const int fd = ::socket(AF_INET, type, 0);
        if (fd < 0)
        {
            throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
        }
        return SocketFd(fd);
    }

    sockaddr_in destination(const Options &options)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(options.port);
        const auto host = normalizeIpv4Host(options.host);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            throw std::runtime_error("invalid IPv4 destination host: " + options.host);
        }
        return addr;
    }

    /**
     * @brief Envoie exactement tous les octets d'une trame sur TCP.
     *
     * TCP peut accepter une écriture partielle même sur une socket bloquante. La boucle avance donc
     * jusqu'à `bytes.size()`. MSG_NOSIGNAL empêche une fermeture distante de terminer brutalement le
     * processus via SIGPIPE ; l'échec est remonté comme exception exploitable par la CLI.
     */
    void sendAll(int fd, const stgs::ByteVector &bytes)
    {
        std::size_t sent = 0U;
        while (sent < bytes.size())
        {
            const ssize_t count = ::send(fd,
                                         bytes.data() + static_cast<std::ptrdiff_t>(sent),
                                         bytes.size() - sent,
                                         MSG_NOSIGNAL);
            if (count < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                throw std::runtime_error("send() failed: " + std::string(std::strerror(errno)));
            }
            if (count == 0)
            {
                throw std::runtime_error("send() returned zero before the frame was fully transmitted");
            }
            sent += static_cast<std::size_t>(count);
        }
    }

    stgs::Status randomStatus(std::mt19937 &rng)
    {
        std::discrete_distribution<int> distribution(
            SimulationStatusWeights.begin(), SimulationStatusWeights.end());
        return static_cast<stgs::Status>(distribution(rng));
    }

    stgs::ByteVector makeRandomPayload(const Options &options, std::mt19937 &rng)
    {
        std::uniform_int_distribution<unsigned int> byteDistribution(
            0U, std::numeric_limits<std::uint8_t>::max());
        stgs::ByteVector payload(options.payloadSize);
        for (auto &byte : payload)
        {
            byte = static_cast<std::uint8_t>(byteDistribution(rng));
        }
        return payload;
    }

    /**
     * @brief Génère un bloc de sinus continu et ajoute éventuellement un bruit blanc gaussien.
     *
     * `blockIndex * signalSamples` détermine l'index absolu du premier point. La phase ne repart donc
     * pas à zéro à chaque trame. Le bruit est ajouté avant l'encodage STGS : il fait partie des
     * données valides et le CRC reste correct, contrairement à `--corrupt`.
     */
    stgs::ByteVector makeSignalPayload(const Options &options, std::mt19937 &rng, std::size_t blockIndex)
    {
        const std::uint64_t startIndex64 = static_cast<std::uint64_t>(blockIndex) *
                                           static_cast<std::uint64_t>(options.signalSamples);
        if (startIndex64 > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error("signal sample index exceeds STGA v1 32-bit range");
        }

        std::normal_distribution<float> noise(0.0F, options.noiseStddev);
        stgs::SignalBlockPayload signal;
        signal.sampleRateHz = options.sampleRateHz;
        signal.frequencyHz = options.signalFrequencyHz;
        signal.nominalAmplitude = options.signalAmplitude;
        signal.startSampleIndex = static_cast<std::uint32_t>(startIndex64);
        signal.samples.reserve(options.signalSamples);

        const double angularStep = TwoPi * static_cast<double>(options.signalFrequencyHz) /
                                   static_cast<double>(options.sampleRateHz);
        for (std::size_t i = 0U; i < options.signalSamples; ++i)
        {
            const std::uint64_t absoluteIndex = startIndex64 + static_cast<std::uint64_t>(i);
            const double clean = static_cast<double>(options.signalAmplitude) *
                                 std::sin(angularStep * static_cast<double>(absoluteIndex));
            const float noisy = static_cast<float>(clean) + noise(rng);
            signal.samples.push_back(noisy);
        }
        return stgs::encodeSignalBlockPayload(signal);
    }

    stgs::TelemetryFrame makeFrame(const Options &options, std::mt19937 &rng, std::size_t sequence)
    {
        std::normal_distribution<float> temperature(
            SimulationTemperatureMeanC, SimulationTemperatureStddevC);

        stgs::TelemetryFrame frame;
        frame.satelliteId = options.satelliteId;
        frame.timestampMs = nowMs();
        frame.temperatureC = temperature(rng);
        frame.batteryPercent = static_cast<std::uint8_t>(
            static_cast<std::size_t>(stgs::MaxBatteryPercent) -
            (sequence % SimulationBatteryCycleLength));
        frame.status = randomStatus(rng);

        switch (options.payloadMode)
        {
        case PayloadMode::RandomBytes:
            frame.payload = makeRandomPayload(options, rng);
            break;
        case PayloadMode::TextMessage:
            frame.payload = stgs::encodeTextMessagePayload(
                stgs::TextMessagePayload{static_cast<std::uint32_t>(sequence), options.message});
            break;
        case PayloadMode::Signal:
            frame.payload = makeSignalPayload(options, rng, sequence);
            break;
        }
        return frame;
    }

    /**
     * @brief Corrompt un octet wire après calcul du CRC afin de simuler une erreur de transport.
     *
     * Le masque XOR 0x5A (01011010b) retourne plusieurs bits sans forcer une valeur fixe. Il est
     * volontairement constant pour rendre la nature de la perturbation reproductible ; la position
     * de l'octet reste aléatoire selon le seed choisi.
     */
    void maybeCorrupt(stgs::ByteVector &bytes, std::mt19937 &rng)
    {
        if (bytes.empty())
        {
            return;
        }
        std::uniform_int_distribution<std::size_t> indexDistribution(0U, bytes.size() - 1U);
        bytes[indexDistribution(rng)] ^= CorruptionXorMask;
    }

    std::string payloadModeName(PayloadMode mode)
    {
        switch (mode)
        {
        case PayloadMode::RandomBytes:
            return "random-bytes";
        case PayloadMode::TextMessage:
            return "text-message";
        case PayloadMode::Signal:
            return "signal";
        }
        return "unknown";
    }

} // namespace

int main(int argc, char **argv)
{
    try
    {
        auto options = parseArgs(argc, argv);
        stgs::TerminalUi ui(options.color);

        if (options.discoverPorts.has_value())
        {
            ui.section("TCP PORT DISCOVERY");
            const auto results = stgs::scanTcpPorts(options.host, *options.discoverPorts, options.discoveryTimeout);
            std::optional<std::uint16_t> selected;
            for (const auto &result : results)
            {
                std::ostringstream value;
                value << stgs::tcpPortStateToString(result.state)
                      << " / " << result.latencyMs << " ms / " << result.detail;
                ui.keyValue("port " + std::to_string(result.port), value.str());
                if (!selected.has_value() && result.state == stgs::TcpPortState::Open)
                {
                    selected = result.port;
                }
            }
            if (!selected.has_value())
            {
                throw std::runtime_error("no OPEN TCP listener found in --discover-ports range");
            }
            options.port = *selected;
            ui.keyValue("selected", std::to_string(options.port));
        }

        ui.section("SATELLITE SIMULATOR");
        ui.keyValue("payload mode", payloadModeName(options.payloadMode));
        ui.keyValue("frames", std::to_string(options.count));
        ui.keyValue("rate", std::to_string(options.rate) + " fps");
        ui.keyValue("satellite", std::to_string(options.satelliteId));
        ui.keyValue("seed", std::to_string(options.seed));
        if (options.transport.has_value())
        {
            ui.keyValue("transport", stgs::transportToString(*options.transport));
            ui.keyValue("destination", options.host + ":" + std::to_string(options.port));
        }
        if (options.payloadMode == PayloadMode::Signal)
        {
            ui.keyValue("signal", std::to_string(options.signalFrequencyHz) + " Hz @ " +
                                      std::to_string(options.sampleRateHz) + " Hz");
            ui.keyValue("noise sigma", std::to_string(options.noiseStddev));
        }
        ui.keyValue("loss probability", std::to_string(options.lossProbability));
        ui.keyValue("CRC corruption probability", std::to_string(options.corruptProbability));

        std::mt19937 rng(options.seed);
        std::uniform_real_distribution<double> probability(0.0, 1.0);
        std::unique_ptr<stgs::FrameFileWriter> capture;
        if (options.outputFile.has_value())
        {
            capture = std::make_unique<stgs::FrameFileWriter>(*options.outputFile);
        }

        std::optional<SocketFd> socket;
        sockaddr_in address{};
        if (options.transport.has_value())
        {
            socket.emplace(createSocket(*options.transport));
            address = destination(options);
            if (*options.transport == stgs::Transport::Tcp)
            {
                ui.section("TCP CONNECTION");
                ui.keyValue("status", "connecting...");
                if (::connect(socket->get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
                {
                    throw std::runtime_error("connect() failed: " + std::string(std::strerror(errno)));
                }
                ui.keyValue("status", "connected");
            }
        }

        const auto delay = options.rate > 0.0
                               ? std::chrono::duration<double>(1.0 / options.rate)
                               : std::chrono::duration<double>(0.0);
        auto nextTick = std::chrono::steady_clock::now();

        std::size_t produced = 0U;
        std::size_t dropped = 0U;
        std::size_t corrupted = 0U;
        ui.section("TRANSMISSION");
        for (std::size_t i = 0U; i < options.count; ++i)
        {
            auto frame = makeFrame(options, rng, i);
            auto bytes = stgs::encodeFrame(frame);

            if (probability(rng) < options.lossProbability)
            {
                ++dropped;
            }
            else
            {
                const bool corrupt = probability(rng) < options.corruptProbability;
                if (corrupt)
                {
                    maybeCorrupt(bytes, rng);
                    ++corrupted;
                }

                if (socket.has_value())
                {
                    if (*options.transport == stgs::Transport::Udp)
                    {
                        const ssize_t sent = ::sendto(socket->get(),
                                                      bytes.data(),
                                                      bytes.size(),
                                                      0,
                                                      reinterpret_cast<const sockaddr *>(&address),
                                                      sizeof(address));
                        if (sent < 0)
                        {
                            throw std::runtime_error("sendto() failed: " + std::string(std::strerror(errno)));
                        }
                        if (static_cast<std::size_t>(sent) != bytes.size())
                        {
                            throw std::runtime_error("UDP datagram was not transmitted atomically");
                        }
                    }
                    else
                    {
                        sendAll(socket->get(), bytes);
                    }
                }
                if (capture)
                {
                    capture->writeFrame(bytes);
                }
                ++produced;
            }

            if (delay.count() > 0.0)
            {
                nextTick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
                std::this_thread::sleep_until(nextTick);
            }
        }

        if (capture)
        {
            capture->flush();
        }

        ui.section("SIMULATOR SUMMARY");
        ui.keyValue("produced", std::to_string(produced));
        ui.keyValue("dropped", std::to_string(dropped));
        ui.keyValue("corrupted", std::to_string(corrupted));
        if (options.outputFile.has_value())
        {
            ui.keyValue("replay", options.outputFile->string());
        }
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        std::cerr << "run with --help for usage\n";
        return 1;
    }
}
