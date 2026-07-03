#include "stgs/BlockingQueue.hpp"
#include "stgs/FrameCodec.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::size_t frames = 200000;
    std::size_t payloadSize = 64;
    std::size_t decoderThreads = std::max(1U, std::thread::hardware_concurrency());
    std::uint32_t seed = 123456789U;
};

void printUsage() {
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

std::string requireValue(int& index, int argc, char** argv) {
    if (index + 1 >= argc) {
        throw std::runtime_error(std::string("missing value after ") + argv[index]);
    }
    ++index;
    return argv[index];
}

Options parseArgs(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            std::exit(0);
        } else if (arg == "--frames") {
            opts.frames = std::stoull(requireValue(i, argc, argv));
        } else if (arg == "--payload-size") {
            opts.payloadSize = std::stoull(requireValue(i, argc, argv));
        } else if (arg == "--decoder-threads") {
            opts.decoderThreads = std::stoull(requireValue(i, argc, argv));
        } else if (arg == "--seed") {
            opts.seed = static_cast<std::uint32_t>(std::stoul(requireValue(i, argc, argv)));
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }
    if (opts.frames == 0U) {
        throw std::runtime_error("--frames must be greater than zero");
    }
    if (opts.payloadSize > stgs::MaxPayloadSize) {
        throw std::runtime_error("--payload-size exceeds MaxPayloadSize");
    }
    if (opts.decoderThreads == 0U) {
        throw std::runtime_error("--decoder-threads must be greater than zero");
    }
    return opts;
}

stgs::TelemetryFrame makeFrame(const Options& opts, std::mt19937& rng, std::size_t index) {
    std::uniform_int_distribution<int> byteDist(0, 255);
    stgs::TelemetryFrame frame;
    frame.satelliteId = static_cast<std::uint16_t>(1000U + (index % 500U));
    frame.timestampMs = 1712345678901ULL + index;
    frame.temperatureC = 20.0F + static_cast<float>(index % 100U) / 10.0F;
    frame.batteryPercent = static_cast<std::uint8_t>(100U - (index % 101U));
    frame.status = stgs::Status::Nominal;
    frame.payload.resize(opts.payloadSize);
    for (auto& byte : frame.payload) {
        byte = static_cast<std::uint8_t>(byteDist(rng));
    }
    return frame;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto opts = parseArgs(argc, argv);
        std::mt19937 rng(opts.seed);
        std::vector<stgs::ByteVector> encoded;
        encoded.reserve(opts.frames);
        for (std::size_t i = 0; i < opts.frames; ++i) {
            encoded.push_back(stgs::encodeFrame(makeFrame(opts, rng, i)));
        }

        stgs::BlockingQueue<stgs::ByteVector> queue;
        std::atomic_size_t decoded{0};
        std::atomic_size_t rejected{0};

        std::vector<std::thread> workers;
        workers.reserve(opts.decoderThreads);
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < opts.decoderThreads; ++i) {
            workers.emplace_back([&] {
                while (auto bytes = queue.pop()) {
                    auto result = stgs::decodeFrame(*bytes);
                    if (std::holds_alternative<stgs::TelemetryFrame>(result)) {
                        ++decoded;
                    } else {
                        ++rejected;
                    }
                }
            });
        }

        for (const auto& frame : encoded) {
            queue.push(frame);
        }
        queue.close();
        for (auto& worker : workers) {
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

        if (decoded.load() != opts.frames || rejected.load() != 0U) {
            return 2;
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        std::cerr << "run with --help for usage\n";
        return 1;
    }
}
