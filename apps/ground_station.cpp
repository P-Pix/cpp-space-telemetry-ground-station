#include "stgs/BlockingQueue.hpp"
#include "stgs/FrameCodec.hpp"
#include "stgs/Logger.hpp"
#include "stgs/NetworkServer.hpp"
#include "stgs/Replay.hpp"
#include "stgs/StationHealth.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic_bool* gRunning = nullptr;

void handleSignal(int) {
    if (gRunning != nullptr) {
        gRunning->store(false);
    }
}

enum class OutputFormat {
    Auto,
    Csv,
    Json
};

struct Options {
    std::optional<stgs::Transport> transport;
    std::string bindAddress = "0.0.0.0";
    std::uint16_t port = 9000;
    std::optional<std::filesystem::path> replayFile;
    double replayRate = 0.0;
    std::filesystem::path outputFile = "telemetry.csv";
    OutputFormat outputFormat = OutputFormat::Auto;
    std::optional<std::filesystem::path> logFile;
    stgs::LogLevel logLevel = stgs::LogLevel::Info;
    std::size_t decoderThreads = 2;
    stgs::StationHealthConfig healthConfig;
};

void printUsage() {
    std::cout << R"(Space Telemetry Ground Station

Usage:
  stgs_ground_station --udp --port 9000 [options]
  stgs_ground_station --tcp --port 9000 [options]
  stgs_ground_station --replay frames.stgf [options]

Options:
  --udp                           Listen for one telemetry frame per UDP datagram.
  --tcp                           Listen for telemetry frames over a TCP byte stream.
  --bind <ipv4>                   Bind address, default 0.0.0.0.
  --port <port>                   Listening port, default 9000.
  --replay <file>                 Replay frames from an STGF binary capture file.
  --replay-rate <fps>             Replay speed in frames/second, 0 means as fast as possible.
  --output <file>                 Output telemetry file, default telemetry.csv.
  --output-format <csv|json>      Output format. Default is inferred from extension, then CSV.
  --log <file>                    Optional log file.
  --log-level <level>             trace, debug, info, warn, error. Default info.
  --decoder-threads <n>           Number of decoder worker threads, default 2.
  --disable-degraded              Disable station degraded-mode monitoring.
  --degraded-window <n>           Health window size, default 100 samples.
  --degraded-min-samples <n>      Minimum samples before state change, default 20.
  --degraded-rejection-rate <p>   Nominal -> Degraded threshold, default 0.10.
  --degraded-recovery-rate <p>    Degraded -> Nominal threshold, default 0.03.
  --degraded-critical-count <n>   Critical telemetry count threshold, default 3.
  --help                          Show this help.
)";
}

std::string requireValue(int& index, int argc, char** argv) {
    if (index + 1 >= argc) {
        throw std::runtime_error(std::string("missing value after ") + argv[index]);
    }
    ++index;
    return argv[index];
}

std::uint16_t parsePort(const std::string& value) {
    const auto port = std::stoul(value);
    if (port == 0 || port > 65535) {
        throw std::runtime_error("port must be in the 1..65535 range");
    }
    return static_cast<std::uint16_t>(port);
}

double parseProbability(const std::string& value, const std::string& name) {
    const auto p = std::stod(value);
    if (p < 0.0 || p > 1.0) {
        throw std::runtime_error(name + " must be between 0 and 1");
    }
    return p;
}

OutputFormat parseOutputFormat(const std::string& value) {
    if (value == "csv") {
        return OutputFormat::Csv;
    }
    if (value == "json") {
        return OutputFormat::Json;
    }
    throw std::runtime_error("--output-format must be csv or json");
}

OutputFormat resolveOutputFormat(OutputFormat requested, const std::filesystem::path& output) {
    if (requested != OutputFormat::Auto) {
        return requested;
    }
    const auto ext = output.extension().string();
    if (ext == ".json") {
        return OutputFormat::Json;
    }
    return OutputFormat::Csv;
}

Options parseArgs(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            std::exit(0);
        } else if (arg == "--udp") {
            opts.transport = stgs::Transport::Udp;
        } else if (arg == "--tcp") {
            opts.transport = stgs::Transport::Tcp;
        } else if (arg == "--bind") {
            opts.bindAddress = requireValue(i, argc, argv);
        } else if (arg == "--port") {
            opts.port = parsePort(requireValue(i, argc, argv));
        } else if (arg == "--replay") {
            opts.replayFile = requireValue(i, argc, argv);
        } else if (arg == "--replay-rate") {
            opts.replayRate = std::stod(requireValue(i, argc, argv));
            if (opts.replayRate < 0.0) {
                throw std::runtime_error("--replay-rate must be >= 0");
            }
        } else if (arg == "--output") {
            opts.outputFile = requireValue(i, argc, argv);
        } else if (arg == "--output-format") {
            opts.outputFormat = parseOutputFormat(requireValue(i, argc, argv));
        } else if (arg == "--log") {
            opts.logFile = requireValue(i, argc, argv);
        } else if (arg == "--log-level") {
            const auto level = stgs::parseLogLevel(requireValue(i, argc, argv));
            if (!level.has_value()) {
                throw std::runtime_error("unknown log level");
            }
            opts.logLevel = *level;
        } else if (arg == "--decoder-threads") {
            opts.decoderThreads = std::stoul(requireValue(i, argc, argv));
            if (opts.decoderThreads == 0) {
                throw std::runtime_error("--decoder-threads must be greater than zero");
            }
        } else if (arg == "--disable-degraded") {
            opts.healthConfig.enabled = false;
        } else if (arg == "--degraded-window") {
            opts.healthConfig.windowSize = std::stoul(requireValue(i, argc, argv));
        } else if (arg == "--degraded-min-samples") {
            opts.healthConfig.minSamples = std::stoul(requireValue(i, argc, argv));
        } else if (arg == "--degraded-rejection-rate") {
            opts.healthConfig.degradedRejectionRate = parseProbability(requireValue(i, argc, argv), "--degraded-rejection-rate");
        } else if (arg == "--degraded-recovery-rate") {
            opts.healthConfig.recoveryRejectionRate = parseProbability(requireValue(i, argc, argv), "--degraded-recovery-rate");
        } else if (arg == "--degraded-critical-count") {
            opts.healthConfig.criticalFramesForDegraded = std::stoul(requireValue(i, argc, argv));
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }

    if (opts.replayFile.has_value() && opts.transport.has_value()) {
        throw std::runtime_error("choose either network mode or replay mode, not both");
    }
    if (!opts.replayFile.has_value() && !opts.transport.has_value()) {
        throw std::runtime_error("choose --udp, --tcp, or --replay");
    }
    if (opts.healthConfig.minSamples > opts.healthConfig.windowSize) {
        throw std::runtime_error("--degraded-min-samples cannot exceed --degraded-window");
    }
    return opts;
}

std::string csvEscape(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') {
            out += "\"\"";
        } else {
            out += ch;
        }
    }
    out += '"';
    return out;
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20U) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

void writeCsvHeader(std::ofstream& out) {
    out << "timestamp_ms,satellite_id,temperature_c,battery_percent,status,payload_len,payload_hex\n";
}

void writeFrameCsv(std::ofstream& out, const stgs::TelemetryFrame& frame) {
    out << frame.timestampMs << ','
        << frame.satelliteId << ','
        << frame.temperatureC << ','
        << static_cast<int>(frame.batteryPercent) << ','
        << stgs::statusToString(frame.status) << ','
        << frame.payload.size() << ','
        << csvEscape(stgs::payloadToHex(frame.payload, frame.payload.size())) << '\n';
}

void writeJsonHeader(std::ofstream& out) {
    out << "[\n";
}

void writeFrameJson(std::ofstream& out, const stgs::TelemetryFrame& frame, bool first) {
    if (!first) {
        out << ",\n";
    }
    out << "  {"
        << "\"timestamp_ms\":" << frame.timestampMs << ','
        << "\"satellite_id\":" << frame.satelliteId << ','
        << "\"temperature_c\":" << std::setprecision(6) << frame.temperatureC << ','
        << "\"battery_percent\":" << static_cast<int>(frame.batteryPercent) << ','
        << "\"status\":\"" << jsonEscape(stgs::statusToString(frame.status)) << "\"," 
        << "\"payload_len\":" << frame.payload.size() << ','
        << "\"payload_hex\":\"" << jsonEscape(stgs::payloadToHex(frame.payload, frame.payload.size())) << "\""
        << '}';
}

void writeJsonFooter(std::ofstream& out) {
    out << "\n]\n";
}

void logTransition(stgs::Logger& logger, const stgs::StationStateTransition& transition) {
    std::ostringstream oss;
    oss << "station state changed: " << stgs::stationStateToString(transition.from)
        << " -> " << stgs::stationStateToString(transition.to)
        << ", samples=" << transition.snapshot.samples
        << ", rejected=" << transition.snapshot.rejected
        << ", rejection_rate=" << transition.snapshot.rejectionRate
        << ", critical=" << transition.snapshot.critical
        << ", reason=" << transition.reason;
    if (transition.to == stgs::StationState::Degraded) {
        logger.warning(oss.str());
    } else {
        logger.info(oss.str());
    }
}

void runReplay(const Options& opts,
               stgs::Logger& logger,
               stgs::BlockingQueue<stgs::ByteVector>& rawQueue,
               std::atomic_ulong& received) {
    stgs::FrameFileReader reader(*opts.replayFile);
    logger.info("replay started from " + opts.replayFile->string());

    const auto delay = opts.replayRate > 0.0
                           ? std::chrono::duration<double>(1.0 / opts.replayRate)
                           : std::chrono::duration<double>(0.0);
    auto nextTick = std::chrono::steady_clock::now();

    while (true) {
        auto frame = reader.readNext();
        if (!frame.has_value()) {
            break;
        }
        rawQueue.push(std::move(*frame));
        ++received;
        if (delay.count() > 0.0) {
            nextTick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
            std::this_thread::sleep_until(nextTick);
        }
    }
    logger.info("replay completed");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto opts = parseArgs(argc, argv);
        const auto outputFormat = resolveOutputFormat(opts.outputFormat, opts.outputFile);
        stgs::Logger logger(opts.logFile, opts.logLevel);
        stgs::StationHealthMonitor health(opts.healthConfig);
        std::atomic_bool running{true};
        gRunning = &running;
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        stgs::BlockingQueue<stgs::ByteVector> rawQueue;
        stgs::BlockingQueue<stgs::TelemetryFrame> decodedQueue;
        std::atomic_ulong received{0};
        std::atomic_ulong decoded{0};
        std::atomic_ulong rejected{0};
        std::atomic_ulong written{0};

        std::ofstream out(opts.outputFile, std::ios::out | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("failed to open output file: " + opts.outputFile.string());
        }
        if (outputFormat == OutputFormat::Json) {
            writeJsonHeader(out);
        } else {
            writeCsvHeader(out);
        }

        std::thread writerThread([&] {
            bool firstJsonFrame = true;
            while (auto frame = decodedQueue.pop()) {
                if (outputFormat == OutputFormat::Json) {
                    writeFrameJson(out, *frame, firstJsonFrame);
                    firstJsonFrame = false;
                } else {
                    writeFrameCsv(out, *frame);
                }
                ++written;
            }
            if (outputFormat == OutputFormat::Json) {
                writeJsonFooter(out);
            }
            out.flush();
        });

        std::vector<std::thread> decoderThreads;
        decoderThreads.reserve(opts.decoderThreads);
        for (std::size_t i = 0; i < opts.decoderThreads; ++i) {
            decoderThreads.emplace_back([&, i] {
                while (auto bytes = rawQueue.pop()) {
                    auto result = stgs::decodeFrame(*bytes);
                    if (std::holds_alternative<stgs::TelemetryFrame>(result)) {
                        auto frame = std::get<stgs::TelemetryFrame>(std::move(result));
                        ++decoded;
                        if (auto transition = health.recordDecoded(frame); transition.has_value()) {
                            logTransition(logger, *transition);
                        }
                        decodedQueue.push(std::move(frame));
                    } else {
                        const auto& err = std::get<stgs::FrameError>(result);
                        ++rejected;
                        if (auto transition = health.recordRejected(); transition.has_value()) {
                            logTransition(logger, *transition);
                        }
                        logger.warning("decoder " + std::to_string(i) + " rejected frame: " +
                                       stgs::errorCodeToString(err.code) + " - " + err.message);
                    }
                }
            });
        }

        if (opts.replayFile.has_value()) {
            runReplay(opts, logger, rawQueue, received);
            running.store(false);
        } else {
            stgs::NetworkConfig config;
            config.transport = *opts.transport;
            config.bindAddress = opts.bindAddress;
            config.port = opts.port;
            stgs::NetworkServer server(config, logger);
            server.run([&](stgs::ByteVector frameBytes) {
                ++received;
                rawQueue.push(std::move(frameBytes));
            }, running);
        }

        rawQueue.close();
        for (auto& thread : decoderThreads) {
            thread.join();
        }
        decodedQueue.close();
        writerThread.join();

        const auto snapshot = health.snapshot();
        std::ostringstream summary;
        summary << "summary received=" << received.load()
                << " decoded=" << decoded.load()
                << " rejected=" << rejected.load()
                << " written=" << written.load()
                << " station_state=" << stgs::stationStateToString(snapshot.state)
                << " rejection_rate=" << snapshot.rejectionRate
                << " output=" << opts.outputFile.string();
        logger.info(summary.str());
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        std::cerr << "run with --help for usage\n";
        return 1;
    }
}
