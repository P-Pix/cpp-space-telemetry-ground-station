/** @file test_network.cpp @brief Tests d'intégration loopback TCP/UDP du serveur réseau. */
#include "support/TestSupport.hpp"
#include "stgs/FrameCodec.hpp"
#include "stgs/Logger.hpp"
#include "stgs/NetworkServer.hpp"
#include "stgs/PortDiagnostics.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace stgs::test
{
    namespace
    {
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
            } });

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
            stoppedPromise.set_value(); });

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
            } });

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

    void registerNetworkTests(TestRegistry &tests)
    {
        tests.emplace_back("tcp_loopback_fragment_and_immediate_close", testTcpLoopbackFragmentAndImmediateClose);
        tests.emplace_back("network_server_cooperative_stop", testNetworkServerCooperativeStop);
        tests.emplace_back("udp_loopback", testUdpLoopback);
    }

} // namespace stgs::test
