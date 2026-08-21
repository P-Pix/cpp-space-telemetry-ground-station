/**
 * @file SimulatorOptions.cpp
 * @brief Parse et valide la configuration du simulateur STGS.
 */
#include "SimulatorOptions.hpp"

#include "stgs/ApplicationPayload.hpp"
#include "stgs/CliParsing.hpp"
#include "stgs/TelemetryFrame.hpp"

#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace stgs::app::simulator
{
    namespace
    {

        inline constexpr std::size_t DefaultSignalFrameCount = 5U;
        inline constexpr std::size_t DefaultMessageFrameCount = 1U;
        inline constexpr std::size_t MaximumGeneratedFrameCount = 10'000'000U;

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

        float parseFiniteFloat(const std::string &value, const std::string &name)
        {
            const double parsed = parseFiniteDouble(value, name);
            if (parsed < -static_cast<double>(std::numeric_limits<float>::max()) ||
                parsed > static_cast<double>(std::numeric_limits<float>::max()))
            {
                throw std::runtime_error(name + " exceeds float range");
            }
            return static_cast<float>(parsed);
        }

    } // namespace

    /**
     * @brief Parse la CLI et effectue les validations qui doivent précéder toute transmission.
     *
     * La fonction vérifie les modes exclusifs, les plages numériques, Nyquist pour le signal, la taille
     * réellement encodable d'un payload STGA et la capacité du compteur absolu d'échantillons. Le
     * simulateur ne commence donc pas une longue émission pour découvrir une incohérence à mi-run.
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
                selectTransport(options, Transport::Udp);
            }
            else if (arg == "--tcp")
            {
                selectTransport(options, Transport::Tcp);
            }
            else if (arg == "--host")
            {
                options.host = requireValue(i, argc, argv);
            }
            else if (arg == "--port")
            {
                options.port = parseUnsigned<std::uint16_t>(
                    requireValue(i, argc, argv), "--port", MinimumNetworkPort, MaximumNetworkPort);
                portExplicit = true;
            }
            else if (arg == "--discover-ports")
            {
                options.discoverPorts = parsePortRange(requireValue(i, argc, argv), "--discover-ports");
            }
            else if (arg == "--discovery-timeout-ms")
            {
                const auto timeout = parseUnsigned<unsigned int>(
                    requireValue(i, argc, argv),
                    "--discovery-timeout-ms",
                    1U,
                    static_cast<unsigned int>(MaximumPortProbeTimeout.count()));
                options.discoveryTimeout = std::chrono::milliseconds(timeout);
            }
            else if (arg == "--count")
            {
                options.count = parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--count", 1U, MaximumGeneratedFrameCount);
                options.countExplicit = true;
            }
            else if (arg == "--rate")
            {
                options.rate = parseFiniteDouble(requireValue(i, argc, argv), "--rate");
                if (options.rate < 0.0)
                {
                    throw std::runtime_error("--rate must be >= 0");
                }
            }
            else if (arg == "--satellite")
            {
                options.satelliteId = parseUnsigned<std::uint16_t>(requireValue(i, argc, argv), "--satellite");
            }
            else if (arg == "--payload-size")
            {
                options.payloadSize = parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--payload-size", 0U, MaxPayloadSize);
                payloadSizeExplicit = true;
            }
            else if (arg == "--loss")
            {
                options.lossProbability = parseProbability(requireValue(i, argc, argv), "--loss");
            }
            else if (arg == "--corrupt")
            {
                options.corruptProbability = parseProbability(requireValue(i, argc, argv), "--corrupt");
            }
            else if (arg == "--seed")
            {
                options.seed = parseUnsigned<std::uint32_t>(requireValue(i, argc, argv), "--seed");
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
                options.sampleRateHz = parseUnsigned<std::uint16_t>(
                    requireValue(i, argc, argv), "--sample-rate", 1U, std::numeric_limits<std::uint16_t>::max());
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
                options.signalSamples = parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--signal-samples", 1U, MaxSignalSamplesPerPayload);
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
            if (!options.transport.has_value() || *options.transport != Transport::Tcp)
            {
                throw std::runtime_error("--discover-ports requires --tcp");
            }
            if (portExplicit)
            {
                throw std::runtime_error("choose either --port or --discover-ports, not both");
            }
            if (!isLoopbackIpv4(options.host))
            {
                throw std::runtime_error("--discover-ports is intentionally restricted to IPv4 loopback");
            }
            if (options.discoverPorts->count() > MaxDiagnosticPortCount)
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
            SignalBlockPayload probe;
            probe.sampleRateHz = options.sampleRateHz;
            probe.frequencyHz = options.signalFrequencyHz;
            probe.nominalAmplitude = options.signalAmplitude;
            probe.samples.assign(options.signalSamples, 0.0F);
            (void)encodeSignalBlockPayload(probe);

            const std::uint64_t lastBlockIndex = static_cast<std::uint64_t>(options.count - 1U);
            const std::uint64_t finalBlockStart = lastBlockIndex * static_cast<std::uint64_t>(options.signalSamples);
            if (finalBlockStart > std::numeric_limits<std::uint32_t>::max())
            {
                throw std::runtime_error(
                    "signal run exceeds STGA v1 32-bit startSampleIndex; reduce --count or --signal-samples");
            }
        }
        if (options.payloadMode == PayloadMode::TextMessage)
        {
            (void)encodeTextMessagePayload(TextMessagePayload{0U, options.message});
        }
        return options;
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

} // namespace stgs::app::simulator
