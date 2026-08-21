/** @file test_application_signal.cpp @brief Tests STGA, signal/DSP et sécurité du rendu terminal. */
#include "support/TestSupport.hpp"
#include "stgs/ApplicationPayload.hpp"
#include "stgs/ByteUtils.hpp"
#include "stgs/SignalProcessing.hpp"
#include "stgs/TerminalUi.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <variant>
#include <vector>

namespace stgs::test
{
    namespace
    {
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
            assertThrows([&]
                         { (void)stgs::encodeSignalBlockPayload(signal); });

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
            assertThrows([&]
                         { (void)stgs::movingAverageFilter(nonFinite, 3U); });
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
            assertThrows([&]
                         { (void)stgs::movingAverageFilter(samples, 4U); });
        }

        void testTerminalSanitizesAnsiInjection()
        {
            const std::string unsafe = std::string("ok") + static_cast<char>(0x1B) + "[31mBAD\n";
            const auto sanitized = stgs::TerminalUi::sanitizeRemoteText(unsafe);
            ASSERT_TRUE(sanitized.find("\\x1B") != std::string::npos);
            ASSERT_TRUE(sanitized.find("\\n") != std::string::npos);
            ASSERT_TRUE(sanitized.find(static_cast<char>(0x1B)) == std::string::npos);
        }

    } // namespace

    void registerApplicationSignalTests(TestRegistry &tests)
    {
        tests.emplace_back("text_payload_round_trip", testTextPayloadRoundTrip);
        tests.emplace_back("signal_payload_round_trip", testSignalPayloadRoundTrip);
        tests.emplace_back("signal_rejects_invalid_metadata", testSignalRejectsInvalidMetadata);
        tests.emplace_back("signal_projection_recovers_known_sine", testSignalProjectionRecoversKnownSine);
        tests.emplace_back("moving_average_rejects_even_window", testMovingAverageRejectsEvenWindow);
        tests.emplace_back("terminal_sanitizes_ansi_injection", testTerminalSanitizesAnsiInjection);
    }

} // namespace stgs::test
