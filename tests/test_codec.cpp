/** @file test_codec.cpp @brief Tests du framing, CRC et codec binaire STGS. */
#include "support/TestSupport.hpp"
#include "stgs/ByteUtils.hpp"
#include "stgs/Crc32.hpp"
#include "stgs/FrameCodec.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <variant>

namespace stgs::test
{
    namespace
    {
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
            assertThrows([&]
                         { (void)stgs::encodeFrame(frame); });

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

    } // namespace

    void registerCodecTests(TestRegistry &tests)
    {
        tests.emplace_back("crc_known_vector", testCrcKnownVector);
        tests.emplace_back("encode_decode_round_trip", testEncodeDecodeRoundTrip);
        tests.emplace_back("bad_magic", testBadMagic);
        tests.emplace_back("bad_crc", testBadCrc);
        tests.emplace_back("non_finite_temperature", testNonFiniteTemperatureRejected);
        tests.emplace_back("length_mismatch", testLengthMismatch);
        tests.emplace_back("stream_extractor_split_and_noise", testStreamExtractorSplitAndNoise);
    }

} // namespace stgs::test
