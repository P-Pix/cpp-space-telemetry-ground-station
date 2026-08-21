/** @file test_replay_health.cpp @brief Tests replay STGF, santé et backpressure. */
#include "support/TestSupport.hpp"
#include "stgs/BlockingQueue.hpp"
#include "stgs/FrameCodec.hpp"
#include "stgs/PortDiagnostics.hpp"
#include "stgs/Replay.hpp"
#include "stgs/StationHealth.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>

namespace stgs::test
{
    namespace
    {
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
                assertThrows([&]
                             { writer.writeFrame(stgs::ByteVector(5U, 0U)); });
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
            assertThrows([&]
                         { stgs::StationHealthMonitor invalidMonitor(invalidConfig); });

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
            auto blockedProducer = std::async(std::launch::async, [&queue]
                                              { return queue.push(2); });
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
            assertThrows([]
                         { (void)stgs::scanTcpPorts("192.0.2.1", stgs::PortRange{9000U, 9001U},
                                                    std::chrono::milliseconds(10)); });
            assertThrows([]
                         { (void)stgs::probeTcpPort("127.0.0.1", 0U, std::chrono::milliseconds(10)); });
            assertThrows([]
                         { (void)stgs::scanTcpPorts("127.0.0.1", stgs::PortRange{9001U, 9000U},
                                                    std::chrono::milliseconds(10)); });
        }

    } // namespace

    void registerReplayHealthTests(TestRegistry &tests)
    {
        tests.emplace_back("replay_round_trip_and_bounds", testReplayRoundTripAndBounds);
        tests.emplace_back("station_health_rejection_and_critical_hysteresis", testStationHealthRejectionAndCriticalHysteresis);
        tests.emplace_back("blocking_queue_and_backpressure", testBlockingQueueAndBackpressure);
        tests.emplace_back("port_diagnostics_are_loopback_only", testPortDiagnosticsAreLoopbackOnly);
    }

} // namespace stgs::test
