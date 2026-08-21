/**
 * @file test_main.cpp
 * @brief Suite légère de tests unitaires et d'intégration loopback du projet STGS.
 *
 * Les tests réseau utilisent uniquement 127.0.0.1 et des ports éphémères. Ils couvrent notamment
 * le framing TCP fragmenté et la fermeture juste après émission, scénario qui aurait détecté le
 * défaut historique de désynchronisation entre le tableau `pollfd` et la liste de clients.
 */

#include "stgs/ApplicationPayload.hpp"
#include "stgs/BlockingQueue.hpp"
#include "stgs/ByteUtils.hpp"
#include "stgs/Crc32.hpp"
#include "stgs/FrameCodec.hpp"
#include "stgs/Logger.hpp"
#include "stgs/NetworkServer.hpp"
#include "stgs/PortDiagnostics.hpp"
#include "stgs/Replay.hpp"
#include "stgs/SignalProcessing.hpp"
#include "stgs/StationHealth.hpp"
#include "stgs/TerminalUi.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace
{

#define ASSERT_TRUE(expr)                                                                                  \
    do                                                                                                     \
    {                                                                                                      \
        if (!(expr))                                                                                       \
        {                                                                                                  \
            throw std::runtime_error(std::string("assertion failed: ") + #expr);                          \
        }                                                                                                  \
    } while (false)

#define ASSERT_EQ(a, b)                                                                                    \
    do                                                                                                     \
    {                                                                                                      \
        const auto lhs = (a);                                                                              \
        const auto rhs = (b);                                                                              \
        if (!(lhs == rhs))                                                                                 \
        {                                                                                                  \
            throw std::runtime_error(std::string("assertion failed: ") + #a + " == " + #b);              \
        }                                                                                                  \
    } while (false)

    void assertNear(double actual, double expected, double tolerance, const char *label)
    {
        if (std::fabs(actual - expected) > tolerance)
        {
            throw std::runtime_error(std::string("assertion failed: ") + label);
        }
    }

    template <typename Fn>
    void assertThrows(Fn &&fn)
    {
        bool thrown = false;
        try
        {
            fn();
        }
        catch (const std::exception &)
        {
            thrown = true;
        }
        ASSERT_TRUE(thrown);
    }

    stgs::TelemetryFrame sampleFrame()
    {
        stgs::TelemetryFrame frame;
        frame.satelliteId = 1337U;
        frame.timestampMs = 1'712'345'678'901ULL;
        frame.temperatureC = 18.75F;
        frame.batteryPercent = 87U;
        frame.status = stgs::Status::Warning;
        frame.payload = {0xDEU, 0xADU, 0xBEU, 0xEFU, 0x01U};
        return frame;
    }

    /** @brief Demande au noyau un port loopback libre puis libère immédiatement la socket. */
    std::uint16_t ephemeralPort(int socketType)
    {
        const int fd = ::socket(AF_INET, socketType, 0);
        if (fd < 0)
        {
            throw std::runtime_error("failed to create ephemeral test socket");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0U);
        if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
        {
            ::close(fd);
            throw std::runtime_error("failed to bind ephemeral test socket");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) < 0)
        {
            ::close(fd);
            throw std::runtime_error("failed to inspect ephemeral test socket");
        }
        const auto port = ntohs(address.sin_port);
        ::close(fd);
        return port;
    }

    int connectLoopback(std::uint16_t port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            throw std::runtime_error("test socket() failed");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
        {
            ::close(fd);
            throw std::runtime_error("test connect() failed");
        }
        return fd;
    }

    void sendExact(int fd, std::span<const std::uint8_t> bytes)
    {
        std::size_t offset = 0U;
        while (offset < bytes.size())
        {
            const ssize_t count = ::send(fd,
                                         bytes.data() + static_cast<std::ptrdiff_t>(offset),
                                         bytes.size() - offset,
                                         MSG_NOSIGNAL);
            if (count <= 0)
            {
                throw std::runtime_error("test send() failed");
            }
            offset += static_cast<std::size_t>(count);
        }
    }

    void rethrowThreadError(const std::exception_ptr &error)
    {
        if (error != nullptr)
        {
            std::rethrow_exception(error);
        }
    }

    void testCrcKnownVector()
    {
        ASSERT_EQ(stgs::crc32("123456789"), stgs::Crc32Check123456789);
    }

    void testEncodeDecodeRoundTrip()
    {
        const auto frame = sampleFrame();
        const auto bytes = stgs::encodeFrame(frame);
        ASSERT_EQ(bytes.size(), stgs::HeaderSize + frame.payload.size() + stgs::CrcSize);

        auto parsed = stgs::decodeFrame(bytes);
        ASSERT_TRUE(std::holds_alternative<stgs::TelemetryFrame>(parsed));
        const auto decoded = std::get<stgs::TelemetryFrame>(parsed);
        ASSERT_EQ(decoded.version, stgs::ProtocolVersion);
        ASSERT_EQ(decoded.satelliteId, frame.satelliteId);
        ASSERT_EQ(decoded.timestampMs, frame.timestampMs);
        ASSERT_TRUE(std::fabs(decoded.temperatureC - frame.temperatureC) < 0.001F);
        ASSERT_EQ(decoded.batteryPercent, frame.batteryPercent);
        ASSERT_EQ(decoded.status, frame.status);
        ASSERT_EQ(decoded.payload, frame.payload);
    }

    void testBadMagic()
    {
        auto bytes = stgs::encodeFrame(sampleFrame());
        bytes[0U] = 0x00U;
        auto parsed = stgs::decodeFrame(bytes);
        ASSERT_TRUE(std::holds_alternative<stgs::FrameError>(parsed));
        ASSERT_EQ(std::get<stgs::FrameError>(parsed).code, stgs::FrameErrorCode::BadMagic);
    }

    void testBadCrc()
    {
        auto bytes = stgs::encodeFrame(sampleFrame());
        bytes[stgs::HeaderSize] ^= 0xFFU;
        auto parsed = stgs::decodeFrame(bytes);
        ASSERT_TRUE(std::holds_alternative<stgs::FrameError>(parsed));
        ASSERT_EQ(std::get<stgs::FrameError>(parsed).code, stgs::FrameErrorCode::BadCrc);
    }

    void testNonFiniteTemperatureRejected()
    {
        auto frame = sampleFrame();
        frame.temperatureC = std::numeric_limits<float>::infinity();
        assertThrows([&] { (void)stgs::encodeFrame(frame); });

        // Construire d'abord une trame valide, puis remplacer les bits de température par +Inf et
        // recalculer le CRC. Le rejet vérifie donc la règle sémantique, pas une corruption du checksum.
        frame.temperatureC = 20.0F;
        auto bytes = stgs::encodeFrame(frame);
        const auto positiveInfinityBits = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());
        stgs::ByteVector encodedInfinity;
        stgs::detail::appendU32BE(encodedInfinity, positiveInfinityBits);
        std::copy(encodedInfinity.begin(), encodedInfinity.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(stgs::TemperatureOffset));

        bytes.resize(bytes.size() - stgs::CrcSize);
        const auto crc = stgs::crc32(bytes);
        stgs::detail::appendU32BE(bytes, crc);

        const auto parsed = stgs::decodeFrame(bytes);
        ASSERT_TRUE(std::holds_alternative<stgs::FrameError>(parsed));
        ASSERT_EQ(std::get<stgs::FrameError>(parsed).code, stgs::FrameErrorCode::InvalidTemperature);
    }

    void testLengthMismatch()
    {
        auto bytes = stgs::encodeFrame(sampleFrame());
        bytes.pop_back();
        auto parsed = stgs::decodeFrame(bytes);
        ASSERT_TRUE(std::holds_alternative<stgs::FrameError>(parsed));
        ASSERT_EQ(std::get<stgs::FrameError>(parsed).code, stgs::FrameErrorCode::LengthMismatch);
    }

    void testStreamExtractorSplitAndNoise()
    {
        const auto frameA = stgs::encodeFrame(sampleFrame());
        auto frameBValue = sampleFrame();
        frameBValue.satelliteId = 99U;
        frameBValue.payload = {1U, 2U, 3U};
        const auto frameB = stgs::encodeFrame(frameBValue);

        stgs::StreamFrameExtractor extractor;
        stgs::ByteVector chunk1 = {0x00U, 0x11U, 0x22U};
        chunk1.insert(chunk1.end(), frameA.begin(), frameA.begin() + 10);
        auto output = extractor.feed(chunk1);
        ASSERT_TRUE(output.empty());

        stgs::ByteVector chunk2(frameA.begin() + 10, frameA.end());
        chunk2.insert(chunk2.end(), frameB.begin(), frameB.end());
        output = extractor.feed(chunk2);
        ASSERT_EQ(output.size(), 2U);
        ASSERT_EQ(output[0U], frameA);
        ASSERT_EQ(output[1U], frameB);
    }

    void testTextPayloadRoundTrip()
    {
        const stgs::TextMessagePayload message{42U, "bonjour STGS"};
        const auto bytes = stgs::encodeTextMessagePayload(message);
        const auto parsed = stgs::decodeApplicationPayload(bytes);
        ASSERT_TRUE(std::holds_alternative<stgs::TextMessagePayload>(parsed));
        const auto decoded = std::get<stgs::TextMessagePayload>(parsed);
        ASSERT_EQ(decoded.sequence, message.sequence);
        ASSERT_EQ(decoded.text, message.text);
    }

    void testSignalPayloadRoundTrip()
    {
        stgs::SignalBlockPayload signal;
        signal.sampleRateHz = 200U;
        signal.frequencyHz = 5.0F;
        signal.nominalAmplitude = 1.5F;
        signal.startSampleIndex = 100U;
        signal.samples = {0.0F, 0.25F, -0.5F, 1.0F};
        const auto bytes = stgs::encodeSignalBlockPayload(signal);
        const auto parsed = stgs::decodeApplicationPayload(bytes);
        ASSERT_TRUE(std::holds_alternative<stgs::SignalBlockPayload>(parsed));
        const auto decoded = std::get<stgs::SignalBlockPayload>(parsed);
        ASSERT_EQ(decoded.sampleRateHz, signal.sampleRateHz);
        ASSERT_EQ(decoded.startSampleIndex, signal.startSampleIndex);
        ASSERT_EQ(decoded.samples, signal.samples);
        assertNear(decoded.frequencyHz, signal.frequencyHz, 0.0001, "signal frequency");
    }

    void testSignalRejectsInvalidMetadata()
    {
        stgs::SignalBlockPayload signal;
        signal.sampleRateHz = 200U;
        signal.frequencyHz = 100.0F; // exactement Nyquist : volontairement invalide.
        signal.nominalAmplitude = 1.0F;
        signal.samples = {0.0F, 1.0F};
        assertThrows([&] { (void)stgs::encodeSignalBlockPayload(signal); });

        signal.frequencyHz = 5.0F;
        auto payload = stgs::encodeSignalBlockPayload(signal);
        stgs::ByteVector encodedNyquist;
        stgs::detail::appendFloatBE(encodedNyquist, 100.0F);
        const std::size_t frequencyOffset = stgs::ApplicationCommonHeaderSize + stgs::U16WireSize;
        std::copy(encodedNyquist.begin(), encodedNyquist.end(),
                  payload.begin() + static_cast<std::ptrdiff_t>(frequencyOffset));
        const auto parsed = stgs::decodeApplicationPayload(payload);
        ASSERT_TRUE(std::holds_alternative<stgs::ApplicationPayloadError>(parsed));
        ASSERT_EQ(std::get<stgs::ApplicationPayloadError>(parsed).code,
                  stgs::ApplicationPayloadErrorCode::InvalidSignalMetadata);

        const std::vector<float> nonFinite{1.0F, std::numeric_limits<float>::infinity()};
        assertThrows([&] { (void)stgs::movingAverageFilter(nonFinite, 3U); });
    }

    void testSignalProjectionRecoversKnownSine()
    {
        constexpr std::uint16_t SampleRate = 200U;
        constexpr float Frequency = 5.0F;
        constexpr std::size_t SampleCount = 400U;
        constexpr double TwoPi = 2.0 * std::numbers::pi_v<double>;
        std::vector<float> raw;
        raw.reserve(SampleCount);
        for (std::size_t i = 0U; i < SampleCount; ++i)
        {
            const double clean = std::sin(TwoPi * static_cast<double>(Frequency) *
                                          static_cast<double>(i) / static_cast<double>(SampleRate));
            const double deterministicNoise = (i % 2U == 0U) ? 0.35 : -0.35;
            raw.push_back(static_cast<float>(clean + deterministicNoise));
        }

        const auto filtered = stgs::sineProjectionFilter(raw, SampleRate, Frequency, 0U);
        const auto metrics = stgs::computeSignalMetrics(raw, filtered, SampleRate, Frequency, 0U);
        assertNear(metrics.estimatedAmplitude, 1.0, 0.05, "estimated sine amplitude");
        ASSERT_TRUE(metrics.residualNoiseRms > 0.2);
        ASSERT_TRUE(metrics.estimatedSnrDb > 3.0);
    }

    void testMovingAverageRejectsEvenWindow()
    {
        const std::vector<float> samples{1.0F, 2.0F, 3.0F};
        assertThrows([&] { (void)stgs::movingAverageFilter(samples, 4U); });
    }

    void testTerminalSanitizesAnsiInjection()
    {
        const std::string unsafe = std::string("ok") + static_cast<char>(0x1B) + "[31mBAD\n";
        const auto sanitized = stgs::TerminalUi::sanitizeRemoteText(unsafe);
        ASSERT_TRUE(sanitized.find("\\x1B") != std::string::npos);
        ASSERT_TRUE(sanitized.find("\\n") != std::string::npos);
        ASSERT_TRUE(sanitized.find(static_cast<char>(0x1B)) == std::string::npos);
    }

    void testReplayRoundTripAndBounds()
    {
        const auto path = std::filesystem::temp_directory_path() / "stgs_replay_test.stgf";
        const auto frameA = stgs::encodeFrame(sampleFrame());
        auto frameBValue = sampleFrame();
        frameBValue.satelliteId = 555U;
        const auto frameB = stgs::encodeFrame(frameBValue);

        {
            stgs::FrameFileWriter writer(path);
            writer.writeFrame(frameA);
            writer.writeFrame(frameB);
            assertThrows([&] { writer.writeFrame(stgs::ByteVector(5U, 0U)); });
            writer.flush();
        }

        stgs::FrameFileReader reader(path);
        const auto readA = reader.readNext();
        const auto readB = reader.readNext();
        const auto readEnd = reader.readNext();
        ASSERT_TRUE(readA.has_value());
        ASSERT_TRUE(readB.has_value());
        ASSERT_TRUE(!readEnd.has_value());
        ASSERT_EQ(*readA, frameA);
        ASSERT_EQ(*readB, frameB);
        std::filesystem::remove(path);
    }

    void testStationHealthRejectionAndCriticalHysteresis()
    {
        stgs::StationHealthConfig invalidConfig;
        invalidConfig.windowSize = 4U;
        invalidConfig.criticalFramesForDegraded = 5U;
        invalidConfig.criticalFramesForRecovery = 1U;
        assertThrows([&] { stgs::StationHealthMonitor invalidMonitor(invalidConfig); });

        stgs::StationHealthConfig config;
        config.windowSize = 6U;
        config.minSamples = 4U;
        config.degradedRejectionRate = 0.5;
        config.recoveryRejectionRate = 0.0;
        config.criticalFramesForDegraded = 3U;
        config.criticalFramesForRecovery = 1U;
        stgs::StationHealthMonitor monitor(config);

        const auto nominal = sampleFrame();
        ASSERT_TRUE(!monitor.recordDecoded(nominal).has_value());
        ASSERT_TRUE(!monitor.recordRejected().has_value());
        ASSERT_TRUE(!monitor.recordDecoded(nominal).has_value());
        auto transition = monitor.recordRejected();
        ASSERT_TRUE(transition.has_value());
        ASSERT_EQ(transition->to, stgs::StationState::Degraded);

        for (std::size_t i = 0U; i < 6U; ++i)
        {
            transition = monitor.recordDecoded(nominal);
        }
        ASSERT_EQ(monitor.state(), stgs::StationState::Nominal);

        auto critical = nominal;
        critical.status = stgs::Status::Critical;
        stgs::StationHealthMonitor criticalMonitor(config);
        ASSERT_TRUE(!criticalMonitor.recordDecoded(critical).has_value());
        ASSERT_TRUE(!criticalMonitor.recordDecoded(critical).has_value());
        ASSERT_TRUE(!criticalMonitor.recordDecoded(nominal).has_value());
        transition = criticalMonitor.recordDecoded(critical);
        ASSERT_TRUE(transition.has_value());
        ASSERT_EQ(transition->to, stgs::StationState::Degraded);
        ASSERT_TRUE(!criticalMonitor.recordDecoded(nominal).has_value());
        ASSERT_TRUE(!criticalMonitor.recordDecoded(nominal).has_value());
        ASSERT_TRUE(!criticalMonitor.recordDecoded(nominal).has_value());
        transition = criticalMonitor.recordDecoded(nominal);
        ASSERT_TRUE(transition.has_value());
        ASSERT_EQ(transition->to, stgs::StationState::Nominal);
    }

    void testBlockingQueueAndBackpressure()
    {
        stgs::BlockingQueue<int> queue(1U);
        ASSERT_TRUE(queue.push(1));
        auto blockedProducer = std::async(std::launch::async, [&queue] { return queue.push(2); });
        ASSERT_EQ(blockedProducer.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
        const auto first = queue.pop();
        ASSERT_TRUE(first.has_value());
        ASSERT_EQ(*first, 1);
        ASSERT_EQ(blockedProducer.wait_for(std::chrono::seconds(1)), std::future_status::ready);
        ASSERT_TRUE(blockedProducer.get());
        queue.close();
        const auto second = queue.pop();
        ASSERT_TRUE(second.has_value());
        ASSERT_EQ(*second, 2);
        ASSERT_TRUE(!queue.pop().has_value());
        ASSERT_TRUE(!queue.push(3));
    }

    void testPortDiagnosticsAreLoopbackOnly()
    {
        ASSERT_TRUE(stgs::isLoopbackIpv4("127.0.0.1"));
        ASSERT_TRUE(stgs::isLoopbackIpv4("127.42.1.9"));
        ASSERT_TRUE(!stgs::isLoopbackIpv4("192.0.2.1"));
        assertThrows([] {
            (void)stgs::scanTcpPorts("192.0.2.1", stgs::PortRange{9000U, 9001U},
                                     std::chrono::milliseconds(10));
        });
        assertThrows([] {
            (void)stgs::probeTcpPort("127.0.0.1", 0U, std::chrono::milliseconds(10));
        });
        assertThrows([] {
            (void)stgs::scanTcpPorts("127.0.0.1", stgs::PortRange{9001U, 9000U},
                                     std::chrono::milliseconds(10));
        });
    }

    void testTcpLoopbackFragmentAndImmediateClose()
    {
        const auto port = ephemeralPort(SOCK_STREAM);
        stgs::Logger logger(std::nullopt, stgs::LogLevel::Error);
        stgs::NetworkConfig config;
        config.transport = stgs::Transport::Tcp;
        config.bindAddress = "127.0.0.1";
        config.port = port;
        config.pollTimeoutMs = 20;

        std::atomic_bool running{true};
        std::promise<stgs::ByteVector> receivedPromise;
        auto receivedFuture = receivedPromise.get_future();
        std::exception_ptr serverError;
        std::thread serverThread([&]
        {
            try
            {
                stgs::NetworkServer server(config, logger);
                server.run([&](stgs::ByteVector bytes)
                {
                    receivedPromise.set_value(std::move(bytes));
                    running.store(false);
                }, running);
            }
            catch (...)
            {
                serverError = std::current_exception();
                running.store(false);
            }
        });

        bool listenerReady = false;
        for (int attempt = 0; attempt < 100 && !listenerReady; ++attempt)
        {
            try
            {
                const auto probe = stgs::probeTcpPort("127.0.0.1", port, std::chrono::milliseconds(20));
                listenerReady = probe.state == stgs::TcpPortState::Open;
            }
            catch (const std::exception &)
            {
            }
            if (!listenerReady)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        ASSERT_TRUE(listenerReady);

        const auto frame = stgs::encodeFrame(sampleFrame());
        const int client = connectLoopback(port);
        constexpr std::size_t FirstFragmentSize = 7U;
        sendExact(client, std::span<const std::uint8_t>(frame.data(), FirstFragmentSize));
        sendExact(client, std::span<const std::uint8_t>(frame.data() + FirstFragmentSize,
                                                       frame.size() - FirstFragmentSize));
        ::close(client); // provoque POLLHUP potentiellement en même temps que les derniers octets.

        ASSERT_EQ(receivedFuture.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        const auto received = receivedFuture.get();
        running.store(false);
        serverThread.join();
        rethrowThreadError(serverError);
        ASSERT_EQ(received, frame);
    }

    void testNetworkServerCooperativeStop()
    {
        const auto port = ephemeralPort(SOCK_STREAM);
        stgs::Logger logger(std::nullopt, stgs::LogLevel::Error);
        stgs::NetworkConfig config;
        config.transport = stgs::Transport::Tcp;
        config.bindAddress = "127.0.0.1";
        config.port = port;
        config.pollTimeoutMs = 50;

        std::atomic_bool running{true};
        stgs::NetworkServer server(config, logger);
        std::promise<void> stoppedPromise;
        auto stoppedFuture = stoppedPromise.get_future();
        std::exception_ptr serverError;

        std::thread serverThread([&]
        {
            try
            {
                server.run([](stgs::ByteVector) {}, running);
            }
            catch (...)
            {
                serverError = std::current_exception();
            }
            stoppedPromise.set_value();
        });

        bool listenerReady = false;
        for (int attempt = 0; attempt < 100 && !listenerReady; ++attempt)
        {
            try
            {
                const auto probe = stgs::probeTcpPort("127.0.0.1", port, std::chrono::milliseconds(20));
                listenerReady = probe.state == stgs::TcpPortState::Open;
            }
            catch (const std::exception &)
            {
            }
            if (!listenerReady)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        if (!listenerReady)
        {
            running.store(false);
            server.stop();
            serverThread.join();
            rethrowThreadError(serverError);
            throw std::runtime_error("TCP listener did not become ready for cooperative stop test");
        }

        server.stop();
        const auto stopStatus = stoppedFuture.wait_for(std::chrono::seconds(1));
        if (stopStatus != std::future_status::ready)
        {
            // Filet de sécurité du test : évite de laisser un thread actif avant de signaler l'échec.
            running.store(false);
            server.stop();
        }
        serverThread.join();
        rethrowThreadError(serverError);
        ASSERT_EQ(stopStatus, std::future_status::ready);
    }

    void testUdpLoopback()
    {
        const auto port = ephemeralPort(SOCK_DGRAM);
        stgs::Logger logger(std::nullopt, stgs::LogLevel::Error);
        stgs::NetworkConfig config;
        config.transport = stgs::Transport::Udp;
        config.bindAddress = "127.0.0.1";
        config.port = port;
        config.pollTimeoutMs = 20;

        std::atomic_bool running{true};
        std::promise<stgs::ByteVector> receivedPromise;
        auto receivedFuture = receivedPromise.get_future();
        std::exception_ptr serverError;
        std::thread serverThread([&]
        {
            try
            {
                stgs::NetworkServer server(config, logger);
                server.run([&](stgs::ByteVector bytes)
                {
                    receivedPromise.set_value(std::move(bytes));
                    running.store(false);
                }, running);
            }
            catch (...)
            {
                serverError = std::current_exception();
                running.store(false);
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const int client = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (client < 0)
        {
            running.store(false);
            serverThread.join();
            throw std::runtime_error("UDP test socket() failed");
        }
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        destination.sin_port = htons(port);
        const auto frame = stgs::encodeFrame(sampleFrame());
        const ssize_t sent = ::sendto(client,
                                      frame.data(),
                                      frame.size(),
                                      0,
                                      reinterpret_cast<const sockaddr *>(&destination),
                                      sizeof(destination));
        ::close(client);
        ASSERT_EQ(static_cast<std::size_t>(sent), frame.size());

        ASSERT_EQ(receivedFuture.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        const auto received = receivedFuture.get();
        running.store(false);
        serverThread.join();
        rethrowThreadError(serverError);
        ASSERT_EQ(received, frame);
    }

} // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"crc_known_vector", testCrcKnownVector},
        {"encode_decode_round_trip", testEncodeDecodeRoundTrip},
        {"bad_magic", testBadMagic},
        {"bad_crc", testBadCrc},
        {"non_finite_temperature", testNonFiniteTemperatureRejected},
        {"length_mismatch", testLengthMismatch},
        {"stream_extractor_split_and_noise", testStreamExtractorSplitAndNoise},
        {"text_payload_round_trip", testTextPayloadRoundTrip},
        {"signal_payload_round_trip", testSignalPayloadRoundTrip},
        {"signal_rejects_invalid_metadata", testSignalRejectsInvalidMetadata},
        {"signal_projection_recovers_known_sine", testSignalProjectionRecoversKnownSine},
        {"moving_average_rejects_even_window", testMovingAverageRejectsEvenWindow},
        {"terminal_sanitizes_ansi_injection", testTerminalSanitizesAnsiInjection},
        {"replay_round_trip_and_bounds", testReplayRoundTripAndBounds},
        {"station_health_rejection_and_critical_hysteresis", testStationHealthRejectionAndCriticalHysteresis},
        {"blocking_queue_and_backpressure", testBlockingQueueAndBackpressure},
        {"port_diagnostics_are_loopback_only", testPortDiagnosticsAreLoopbackOnly},
        {"tcp_loopback_fragment_and_immediate_close", testTcpLoopbackFragmentAndImmediateClose},
        {"network_server_cooperative_stop", testNetworkServerCooperativeStop},
        {"udp_loopback", testUdpLoopback},
    };

    int failed = 0;
    for (const auto &[name, function] : tests)
    {
        try
        {
            function();
            std::cout << "[PASS] " << name << '\n';
        }
        catch (const std::exception &ex)
        {
            ++failed;
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
        }
    }

    if (failed != 0)
    {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test(s) passed\n";
    return 0;
}
