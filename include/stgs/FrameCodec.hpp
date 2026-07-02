#pragma once

#include "stgs/TelemetryFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace stgs {

enum class FrameErrorCode {
    TooShort,
    BadMagic,
    UnsupportedVersion,
    PayloadTooLarge,
    LengthMismatch,
    BadCrc,
    InvalidBattery,
    InvalidStatus
};

struct FrameError {
    FrameErrorCode code;
    std::string message;
};

using FrameParseResult = std::variant<TelemetryFrame, FrameError>;

ByteVector encodeFrame(const TelemetryFrame& frame);
FrameParseResult decodeFrame(std::span<const std::uint8_t> bytes);
const char* errorCodeToString(FrameErrorCode code) noexcept;

// Reassembles complete binary telemetry frames from a TCP byte stream.
// UDP does not need this because each datagram is already a candidate frame.
class StreamFrameExtractor {
public:
    std::vector<ByteVector> feed(std::span<const std::uint8_t> bytes);
    void clear();
    [[nodiscard]] std::size_t bufferedBytes() const noexcept;

private:
    ByteVector buffer_;
};

} // namespace stgs
