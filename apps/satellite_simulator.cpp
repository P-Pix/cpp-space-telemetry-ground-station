#include "stgs/FrameCodec.hpp"
#include "stgs/NetworkServer.hpp"
#include "stgs/Replay.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct Options {
    std::optional<stgs::Transport> transport;
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    std::size_t count = 1000;
    double rate = 100.0;
    std::uint16_t satelliteId = 42;
    std::size_t payloadSize = 32;
    double lossProbability = 0.0;
    double corruptProbability = 0.0;
    std::uint32_t seed = std::random_device{}();
    std::optional<std::filesystem::path> outputFile;
};

void printUsage() {
    std::cout << R"(Satellite Telemetry Simulator

Usage:
  stgs_satellite_simulator --udp --host 127.0.0.1 --port 9000 [options]
  stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 [options]
  stgs_satellite_simulator --output-file frames.stgf [options]

Options:
  --udp                    Send frames as UDP datagrams.
  --tcp                    Send frames over a TCP byte stream.
  --host <ipv4>            Destination host, default 127.0.0.1.
  --port <port>            Destination port, default 9000.
  --count <n>              Number of frames to produce, default 1000.
  --rate <fps>             Frames/second, 0 means as fast as possible.
  --satellite <id>         Satellite ID, default 42.
  --payload-size <bytes>   Payload bytes per frame, default 32, max 4096.
  --loss <p>               Drop probability between 0 and 1, default 0.
  --corrupt <p>            Byte corruption probability between 0 and 1, default 0.
  --seed <n>               Deterministic RNG seed.
  --output-file <file>     Write transmitted frames to an STGF replay file.
  --help                   Show this help.
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
        } else if (arg == "--host") {
            opts.host = requireValue(i, argc, argv);
        } else if (arg == "--port") {
            opts.port = parsePort(requireValue(i, argc, argv));
        } else if (arg == "--count") {
            opts.count = std::stoull(requireValue(i, argc, argv));
        } else if (arg == "--rate") {
            opts.rate = std::stod(requireValue(i, argc, argv));
            if (opts.rate < 0.0) {
                throw std::runtime_error("--rate must be >= 0");
            }
        } else if (arg == "--satellite") {
            opts.satelliteId = static_cast<std::uint16_t>(std::stoul(requireValue(i, argc, argv)));
        } else if (arg == "--payload-size") {
            opts.payloadSize = std::stoul(requireValue(i, argc, argv));
            if (opts.payloadSize > stgs::MaxPayloadSize) {
                throw std::runtime_error("payload size exceeds MaxPayloadSize");
            }
        } else if (arg == "--loss") {
            opts.lossProbability = parseProbability(requireValue(i, argc, argv), "--loss");
        } else if (arg == "--corrupt") {
            opts.corruptProbability = parseProbability(requireValue(i, argc, argv), "--corrupt");
        } else if (arg == "--seed") {
            opts.seed = static_cast<std::uint32_t>(std::stoul(requireValue(i, argc, argv)));
        } else if (arg == "--output-file") {
            opts.outputFile = requireValue(i, argc, argv);
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }

    if (!opts.transport.has_value() && !opts.outputFile.has_value()) {
        throw std::runtime_error("choose --udp, --tcp, and/or --output-file");
    }
    return opts;
}

std::uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

int createSocket(const Options& opts) {
    const int type = opts.transport == stgs::Transport::Udp ? SOCK_DGRAM : SOCK_STREAM;
    int fd = ::socket(AF_INET, type, 0);
    if (fd < 0) {
        throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
    }
    return fd;
}

sockaddr_in destination(const Options& opts) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opts.port);
    if (::inet_pton(AF_INET, opts.host.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid IPv4 destination host: " + opts.host);
    }
    return addr;
}

void sendAll(int fd, const stgs::ByteVector& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("send() failed: " + std::string(std::strerror(errno)));
        }
        sent += static_cast<std::size_t>(n);
    }
}

stgs::Status randomStatus(std::mt19937& rng) {
    std::discrete_distribution<int> dist({90, 7, 2, 1});
    return static_cast<stgs::Status>(dist(rng));
}

stgs::TelemetryFrame makeFrame(const Options& opts, std::mt19937& rng, std::size_t sequence) {
    std::normal_distribution<float> tempDist(22.0F, 4.0F);
    std::uniform_int_distribution<int> byteDist(0, 255);

    stgs::TelemetryFrame frame;
    frame.satelliteId = opts.satelliteId;
    frame.timestampMs = nowMs();
    frame.temperatureC = tempDist(rng);
    frame.batteryPercent = static_cast<std::uint8_t>(100U - (sequence % 101U));
    frame.status = randomStatus(rng);
    frame.payload.resize(opts.payloadSize);
    for (auto& byte : frame.payload) {
        byte = static_cast<std::uint8_t>(byteDist(rng));
    }
    return frame;
}

void maybeCorrupt(stgs::ByteVector& bytes, std::mt19937& rng) {
    if (bytes.empty()) {
        return;
    }
    std::uniform_int_distribution<std::size_t> indexDist(0, bytes.size() - 1U);
    const auto index = indexDist(rng);
    bytes[index] ^= 0x5AU;
}

} // namespace

int main(int argc, char** argv) {
    int fd = -1;
    try {
        const auto opts = parseArgs(argc, argv);
        std::mt19937 rng(opts.seed);
        std::uniform_real_distribution<double> probability(0.0, 1.0);
        std::unique_ptr<stgs::FrameFileWriter> capture;
        if (opts.outputFile.has_value()) {
            capture = std::make_unique<stgs::FrameFileWriter>(*opts.outputFile);
        }

        sockaddr_in addr{};
        if (opts.transport.has_value()) {
            fd = createSocket(opts);
            addr = destination(opts);
            if (*opts.transport == stgs::Transport::Tcp) {
                if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
                    throw std::runtime_error("connect() failed: " + std::string(std::strerror(errno)));
                }
            }
        }

        const auto delay = opts.rate > 0.0
                               ? std::chrono::duration<double>(1.0 / opts.rate)
                               : std::chrono::duration<double>(0.0);
        auto nextTick = std::chrono::steady_clock::now();

        std::size_t produced = 0;
        std::size_t dropped = 0;
        std::size_t corrupted = 0;
        for (std::size_t i = 0; i < opts.count; ++i) {
            auto frame = makeFrame(opts, rng, i);
            auto bytes = stgs::encodeFrame(frame);

            if (probability(rng) < opts.lossProbability) {
                ++dropped;
            } else {
                const bool corrupt = probability(rng) < opts.corruptProbability;
                if (corrupt) {
                    maybeCorrupt(bytes, rng);
                    ++corrupted;
                }

                if (opts.transport.has_value()) {
                    if (*opts.transport == stgs::Transport::Udp) {
                        const ssize_t n = ::sendto(fd, bytes.data(), bytes.size(), 0,
                                                   reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
                        if (n < 0) {
                            throw std::runtime_error("sendto() failed: " + std::string(std::strerror(errno)));
                        }
                    } else {
                        sendAll(fd, bytes);
                    }
                }
                if (capture) {
                    capture->writeFrame(bytes);
                }
                ++produced;
            }

            if (delay.count() > 0.0) {
                nextTick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
                std::this_thread::sleep_until(nextTick);
            }
        }

        if (fd >= 0) {
            ::close(fd);
        }
        std::cout << "simulator summary produced=" << produced
                  << " dropped=" << dropped
                  << " corrupted=" << corrupted << '\n';
        return 0;
    } catch (const std::exception& ex) {
        if (fd >= 0) {
            ::close(fd);
        }
        std::cerr << "error: " << ex.what() << '\n';
        std::cerr << "run with --help for usage\n";
        return 1;
    }
}
