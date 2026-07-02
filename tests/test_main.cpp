#include "stgs/BlockingQueue.hpp"
#include "stgs/Crc32.hpp"
#include "stgs/FrameCodec.hpp"
#include "stgs/Replay.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

#define ASSERT_TRUE(expr) do { if (!(expr)) { throw std::runtime_error(std::string("assertion failed: ") + #expr); } } while (false)
#define ASSERT_EQ(a, b) do { const auto lhs = (a); const auto rhs = (b); if (!(lhs == rhs)) { throw std::runtime_error(std::string("assertion failed: ") + #a + " == " + #b); } } while (false)

stgs::TelemetryFrame sampleFrame() {
    stgs::TelemetryFrame frame;
    frame.satelliteId = 1337;
    frame.timestampMs = 1712345678901ULL;
    frame.temperatureC = 18.75F;
    frame.batteryPercent = 87;
    frame.status = stgs::Status::Warning;
    frame.payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    return frame;
}

void testCrcKnownVector() {
    ASSERT_EQ(stgs::crc32("123456789"), 0xCBF43926U);
}

void testEncodeDecodeRoundTrip() {
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

void testBadMagic() {
    auto bytes = stgs::encodeFrame(sampleFrame());
    bytes[0] = 0x00;
    auto parsed = stgs::decodeFrame(bytes);
    ASSERT_TRUE(std::holds_alternative<stgs::FrameError>(parsed));
    ASSERT_EQ(std::get<stgs::FrameError>(parsed).code, stgs::FrameErrorCode::BadMagic);
}

void testBadCrc() {
    auto bytes = stgs::encodeFrame(sampleFrame());
    bytes[stgs::HeaderSize] ^= 0xFF;
    auto parsed = stgs::decodeFrame(bytes);
    ASSERT_TRUE(std::holds_alternative<stgs::FrameError>(parsed));
    ASSERT_EQ(std::get<stgs::FrameError>(parsed).code, stgs::FrameErrorCode::BadCrc);
}

void testLengthMismatch() {
    auto bytes = stgs::encodeFrame(sampleFrame());
    bytes.pop_back();
    auto parsed = stgs::decodeFrame(bytes);
    ASSERT_TRUE(std::holds_alternative<stgs::FrameError>(parsed));
    ASSERT_EQ(std::get<stgs::FrameError>(parsed).code, stgs::FrameErrorCode::LengthMismatch);
}

void testStreamExtractorSplitAndNoise() {
    const auto frameA = stgs::encodeFrame(sampleFrame());
    auto frameBValue = sampleFrame();
    frameBValue.satelliteId = 99;
    frameBValue.payload = {1, 2, 3};
    const auto frameB = stgs::encodeFrame(frameBValue);

    stgs::StreamFrameExtractor extractor;
    stgs::ByteVector chunk1 = {0x00, 0x11, 0x22};
    chunk1.insert(chunk1.end(), frameA.begin(), frameA.begin() + 10);
    auto out = extractor.feed(chunk1);
    ASSERT_TRUE(out.empty());

    stgs::ByteVector chunk2(frameA.begin() + 10, frameA.end());
    chunk2.insert(chunk2.end(), frameB.begin(), frameB.end());
    out = extractor.feed(chunk2);
    ASSERT_EQ(out.size(), 2U);
    ASSERT_EQ(out[0], frameA);
    ASSERT_EQ(out[1], frameB);
}

void testReplayRoundTrip() {
    const auto path = std::filesystem::temp_directory_path() / "stgs_replay_test.stgf";
    const auto frameA = stgs::encodeFrame(sampleFrame());
    auto frameBValue = sampleFrame();
    frameBValue.satelliteId = 555;
    const auto frameB = stgs::encodeFrame(frameBValue);

    {
        stgs::FrameFileWriter writer(path);
        writer.writeFrame(frameA);
        writer.writeFrame(frameB);
    }

    stgs::FrameFileReader reader(path);
    auto readA = reader.readNext();
    auto readB = reader.readNext();
    auto readEnd = reader.readNext();
    ASSERT_TRUE(readA.has_value());
    ASSERT_TRUE(readB.has_value());
    ASSERT_TRUE(!readEnd.has_value());
    ASSERT_EQ(*readA, frameA);
    ASSERT_EQ(*readB, frameB);
    std::filesystem::remove(path);
}

void testBlockingQueue() {
    stgs::BlockingQueue<int> queue;
    auto future = std::async(std::launch::async, [&queue] {
        auto value = queue.pop();
        ASSERT_TRUE(value.has_value());
        ASSERT_EQ(*value, 42);
        auto closed = queue.pop();
        ASSERT_TRUE(!closed.has_value());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(queue.push(42));
    queue.close();
    future.get();
    ASSERT_TRUE(!queue.push(1));
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"crc_known_vector", testCrcKnownVector},
        {"encode_decode_round_trip", testEncodeDecodeRoundTrip},
        {"bad_magic", testBadMagic},
        {"bad_crc", testBadCrc},
        {"length_mismatch", testLengthMismatch},
        {"stream_extractor_split_and_noise", testStreamExtractorSplitAndNoise},
        {"replay_round_trip", testReplayRoundTrip},
        {"blocking_queue", testBlockingQueue},
    };

    int failed = 0;
    for (const auto& [name, fn] : tests) {
        try {
            fn();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
        }
    }

    if (failed != 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test(s) passed\n";
    return 0;
}
