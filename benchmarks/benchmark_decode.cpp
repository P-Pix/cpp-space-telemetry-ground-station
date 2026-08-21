/**
 * @file benchmark_decode.cpp
 * @brief Implémente le benchmark isolé du décodage et du pipeline de files.
 *
 * La mesure écarte volontairement réseau et disque pour caractériser le coût CPU du traitement de trames.
 */

#include "stgs/BlockingQueue.hpp"
#include "stgs/CliParsing.hpp"
#include "stgs/FrameCodec.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

    // Valeurs de scénario reproductibles : elles décrivent la charge du benchmark, pas le protocole STGS.
    inline constexpr std::size_t DefaultBenchmarkFrames = 200'000U;
    inline constexpr std::size_t DefaultBenchmarkPayloadBytes = 64U;
    // File bornée assez large pour découpler brièvement producteur/workers sans masquer la backpressure.
    inline constexpr std::size_t BenchmarkQueueCapacity = 4096U;
    inline constexpr std::size_t MaximumBenchmarkFrames = 10'000'000U;
    inline constexpr std::size_t MaximumBenchmarkDecoderThreads = 256U;
    inline constexpr std::uint32_t DefaultBenchmarkSeed = 123'456'789U;

    // Jeu de télémétrie synthétique déterministe utilisé uniquement pour nourrir le décodeur.
    inline constexpr std::uint16_t BenchmarkSatelliteBaseId = 1000U;
    inline constexpr std::size_t BenchmarkSatellitePoolSize = 500U;
    inline constexpr std::uint64_t BenchmarkTimestampBaseMs = 1'712'345'678'901ULL;
    inline constexpr float BenchmarkTemperatureBaseC = 20.0F;
    inline constexpr std::size_t BenchmarkTemperatureCycleSteps = 100U;
    inline constexpr float BenchmarkTemperatureStepC = 0.1F;
    inline constexpr std::size_t BenchmarkBatteryCycleStates = 101U; // valeurs 100..0 incluses.

    struct Options
    {
        std::size_t frames = DefaultBenchmarkFrames;
        std::size_t payloadSize = DefaultBenchmarkPayloadBytes;
        std::size_t decoderThreads = std::max(1U, std::thread::hardware_concurrency());
        std::uint32_t seed = DefaultBenchmarkSeed;
    };

    void printUsage()
    {
        std::cout << R"(STGS decode pipeline benchmark

Usage:
  stgs_benchmark_decode [options]

Options:
  --frames <n>            Number of frames, default 200000.
  --payload-size <bytes>  Payload bytes per frame, default 64, max 4096.
  --decoder-threads <n>   Number of decoder workers, default hardware concurrency.
  --seed <n>              Deterministic payload seed, default 123456789.
  --help                  Show this help.
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

    /**
     * @brief Parse strictement la charge du benchmark avant d'allouer les trames.
     *
     * Les mêmes helpers que les exécutables de production sont utilisés : une valeur avec suffixe,
     * un dépassement entier ou un nombre de workers nul est donc refusé au lieu d'être tronqué.
     * La borne sur le nombre de trames évite qu'une faute de frappe transforme le benchmark en
     * allocation mémoire démesurée puisqu'il pré-génère volontairement l'ensemble du jeu de données.
     */
    Options parseArgs(int argc, char **argv)
    {
        Options opts;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h")
            {
                printUsage();
                std::exit(0);
            }
            else if (arg == "--frames")
            {
                opts.frames = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--frames", 1U, MaximumBenchmarkFrames);
            }
            else if (arg == "--payload-size")
            {
                opts.payloadSize = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--payload-size", 0U, stgs::MaxPayloadSize);
            }
            else if (arg == "--decoder-threads")
            {
                opts.decoderThreads = stgs::parseUnsigned<std::size_t>(
                    requireValue(i, argc, argv), "--decoder-threads", 1U, MaximumBenchmarkDecoderThreads);
            }
            else if (arg == "--seed")
            {
                opts.seed = stgs::parseUnsigned<std::uint32_t>(requireValue(i, argc, argv), "--seed");
            }
            else
            {
                throw std::runtime_error("unknown option: " + arg);
            }
        }
        return opts;
    }

    /**
     * @brief Construit une trame valide et déterministe pour isoler le coût du pipeline de décodage.
     *
     * Les variations de satellite, température et batterie empêchent le benchmark de ne traiter qu'un
     * buffer constant, sans prétendre simuler une mission réelle. Les cycles et l'époque de départ sont
     * des données de charge nommées ci-dessus ; aucune de ces valeurs n'est une constante du protocole.
     *
     * @param opts Configuration de payload du benchmark.
     * @param rng Générateur déterministe utilisé pour les octets de payload.
     * @param index Index de la trame synthétique dans le jeu pré-généré.
     * @return TelemetryFrame conforme, prête à être encodée par le codec réel.
     */
    stgs::TelemetryFrame makeFrame(const Options &opts, std::mt19937 &rng, std::size_t index)
    {
        std::uniform_int_distribution<unsigned int> byteDist(
            0U, static_cast<unsigned int>(std::numeric_limits<std::uint8_t>::max()));

        stgs::TelemetryFrame frame;
        const auto satelliteOffset = index % BenchmarkSatellitePoolSize;
        frame.satelliteId = static_cast<std::uint16_t>(
            static_cast<std::size_t>(BenchmarkSatelliteBaseId) + satelliteOffset);
        frame.timestampMs = BenchmarkTimestampBaseMs + static_cast<std::uint64_t>(index);
        frame.temperatureC = BenchmarkTemperatureBaseC +
                             static_cast<float>(index % BenchmarkTemperatureCycleSteps) * BenchmarkTemperatureStepC;
        frame.batteryPercent = static_cast<std::uint8_t>(
            static_cast<std::size_t>(stgs::MaxBatteryPercent) - (index % BenchmarkBatteryCycleStates));
        frame.status = stgs::Status::Nominal;
        frame.payload.resize(opts.payloadSize);
        for (auto &byte : frame.payload)
        {
            byte = static_cast<std::uint8_t>(byteDist(rng));
        }
        return frame;
    }

} // namespace

int main(int argc, char **argv)
{
    try
    {
        const auto opts = parseArgs(argc, argv);
        std::mt19937 rng(opts.seed);
        std::vector<stgs::ByteVector> encoded;
        encoded.reserve(opts.frames);
        for (std::size_t i = 0; i < opts.frames; ++i)
        {
            encoded.push_back(stgs::encodeFrame(makeFrame(opts, rng, i)));
        }

        stgs::BlockingQueue<stgs::ByteVector> queue(BenchmarkQueueCapacity);
        std::atomic_size_t decoded{0};
        std::atomic_size_t rejected{0};

        std::vector<std::thread> workers;
        workers.reserve(opts.decoderThreads);
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < opts.decoderThreads; ++i)
        {
            workers.emplace_back([&]
                                 {
                while (auto bytes = queue.pop()) {
                    auto result = stgs::decodeFrame(*bytes);
                    if (std::holds_alternative<stgs::TelemetryFrame>(result)) {
                        ++decoded;
                    } else {
                        ++rejected;
                    }
                } });
        }

        for (const auto &frame : encoded)
        {
            queue.push(frame);
        }
        queue.close();
        for (auto &worker : workers)
        {
            worker.join();
        }
        const auto end = std::chrono::steady_clock::now();
        const auto seconds = std::chrono::duration<double>(end - start).count();
        const auto fps = static_cast<double>(decoded.load() + rejected.load()) / seconds;

        std::cout << "benchmark=decode_pipeline"
                  << " frames=" << opts.frames
                  << " payload_size=" << opts.payloadSize
                  << " decoder_threads=" << opts.decoderThreads
                  << " decoded=" << decoded.load()
                  << " rejected=" << rejected.load()
                  << " duration_seconds=" << seconds
                  << " frames_per_second=" << fps << '\n';

        if (decoded.load() != opts.frames || rejected.load() != 0U)
        {
            return 2;
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
