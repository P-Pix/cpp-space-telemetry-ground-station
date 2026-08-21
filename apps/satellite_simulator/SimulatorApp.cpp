/**
 * @file SimulatorApp.cpp
 * @brief Assemble découverte, génération, transport et capture du simulateur STGS.
 */
#include "SimulatorApp.hpp"

#include "SimulatorFrameFactory.hpp"
#include "SimulatorSocket.hpp"
#include "stgs/FrameCodec.hpp"
#include "stgs/PortDiagnostics.hpp"
#include "stgs/Replay.hpp"
#include "stgs/TerminalUi.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>

namespace stgs::app::simulator
{
    namespace
    {

        void discoverPort(Options &options, TerminalUi &ui)
        {
            if (!options.discoverPorts.has_value())
            {
                return;
            }

            ui.section("TCP PORT DISCOVERY");
            const auto results = scanTcpPorts(options.host, *options.discoverPorts, options.discoveryTimeout);
            std::optional<std::uint16_t> selected;
            for (const auto &result : results)
            {
                std::ostringstream value;
                value << tcpPortStateToString(result.state)
                      << " / " << result.latencyMs << " ms / " << result.detail;
                ui.keyValue("port " + std::to_string(result.port), value.str());
                if (!selected.has_value() && result.state == TcpPortState::Open)
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

        void renderConfiguration(const Options &options, TerminalUi &ui)
        {
            ui.section("SATELLITE SIMULATOR");
            ui.keyValue("payload mode", payloadModeName(options.payloadMode));
            ui.keyValue("frames", std::to_string(options.count));
            ui.keyValue("rate", std::to_string(options.rate) + " fps");
            ui.keyValue("satellite", std::to_string(options.satelliteId));
            ui.keyValue("seed", std::to_string(options.seed));
            if (options.transport.has_value())
            {
                ui.keyValue("transport", transportToString(*options.transport));
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
        }

        void sendUdpFrame(int fd, const sockaddr_in &address, const ByteVector &bytes)
        {
            const ssize_t sent = ::sendto(fd,
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

    } // namespace

    int run(Options options)
    {
        TerminalUi ui(options.color);
        discoverPort(options, ui);
        renderConfiguration(options, ui);

        std::mt19937 rng(options.seed);
        std::uniform_real_distribution<double> probability(0.0, 1.0);
        std::unique_ptr<FrameFileWriter> capture;
        if (options.outputFile.has_value())
        {
            capture = std::make_unique<FrameFileWriter>(*options.outputFile);
        }

        std::optional<SocketFd> socket;
        sockaddr_in address{};
        if (options.transport.has_value())
        {
            socket.emplace(createSocket(*options.transport));
            address = destination(options);
            if (*options.transport == Transport::Tcp)
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
            auto bytes = encodeFrame(frame);

            if (probability(rng) < options.lossProbability)
            {
                ++dropped;
            }
            else
            {
                if (probability(rng) < options.corruptProbability)
                {
                    maybeCorrupt(bytes, rng);
                    ++corrupted;
                }

                if (socket.has_value())
                {
                    if (*options.transport == Transport::Udp)
                    {
                        sendUdpFrame(socket->get(), address, bytes);
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

} // namespace stgs::app::simulator
