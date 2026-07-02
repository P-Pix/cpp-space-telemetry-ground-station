#include "stgs/FrameCodec.hpp"

#include "stgs/ByteUtils.hpp"
#include "stgs/Crc32.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace stgs {
namespace {

FrameError makeError(FrameErrorCode code, std::string message) {
    return FrameError{code, std::move(message)};
}

ByteVector magicBytes() {
    ByteVector bytes;
    detail::appendU32BE(bytes, FrameMagic);
    return bytes;
}

} // namespace

std::string statusToString(Status status) {
    switch (status) {
    case Status::Nominal:
        return "NOMINAL";
    case Status::Warning:
        return "WARNING";
    case Status::Critical:
        return "CRITICAL";
    case Status::SafeMode:
        return "SAFE_MODE";
    }
    return "UNKNOWN";
}

std::string payloadToHex(const ByteVector& payload, std::size_t maxBytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    const auto displayed = std::min(payload.size(), maxBytes);
    for (std::size_t i = 0; i < displayed; ++i) {
        if (i > 0U) {
            oss << ' ';
        }
        oss << std::setw(2) << static_cast<int>(payload[i]);
    }
    if (payload.size() > displayed) {
        oss << " ...(" << std::dec << payload.size() << " bytes)";
    }
    return oss.str();
}

std::string toString(const TelemetryFrame& frame) {
    std::ostringstream oss;
    oss << "sat=" << frame.satelliteId
        << " ts_ms=" << frame.timestampMs
        << " temp_c=" << std::fixed << std::setprecision(2) << frame.temperatureC
        << " battery=" << static_cast<int>(frame.batteryPercent) << "%"
        << " status=" << statusToString(frame.status)
        << " payload_len=" << frame.payload.size();
    return oss.str();
}

ByteVector encodeFrame(const TelemetryFrame& frame) {
    if (frame.payload.size() > MaxPayloadSize) {
        throw std::invalid_argument("payload exceeds MaxPayloadSize");
    }
    if (frame.version != ProtocolVersion) {
        throw std::invalid_argument("unsupported protocol version");
    }
    if (frame.batteryPercent > 100U) {
        throw std::invalid_argument("battery percent must be between 0 and 100");
    }

    const auto statusByte = static_cast<std::uint8_t>(frame.status);
    if (statusByte > static_cast<std::uint8_t>(Status::SafeMode)) {
        throw std::invalid_argument("invalid status value");
    }

    ByteVector bytes;
    bytes.reserve(HeaderSize + frame.payload.size() + CrcSize);
    detail::appendU32BE(bytes, FrameMagic);
    detail::appendU8(bytes, frame.version);
    detail::appendU16BE(bytes, frame.satelliteId);
    detail::appendU64BE(bytes, frame.timestampMs);
    detail::appendFloatBE(bytes, frame.temperatureC);
    detail::appendU8(bytes, frame.batteryPercent);
    detail::appendU8(bytes, statusByte);
    detail::appendU16BE(bytes, static_cast<std::uint16_t>(frame.payload.size()));
    bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());

    const auto crc = crc32(bytes);
    detail::appendU32BE(bytes, crc);
    return bytes;
}

FrameParseResult decodeFrame(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < MinFrameSize) {
        return makeError(FrameErrorCode::TooShort, "frame is shorter than the minimum header + CRC size");
    }

    const auto magic = detail::readU32BE(bytes, 0U);
    if (magic != FrameMagic) {
        return makeError(FrameErrorCode::BadMagic, "invalid frame magic; expected ASCII STGS");
    }

    const auto version = bytes[4U];
    if (version != ProtocolVersion) {
        return makeError(FrameErrorCode::UnsupportedVersion, "unsupported protocol version");
    }

    const auto payloadLength = detail::readU16BE(bytes, 21U);
    if (payloadLength > MaxPayloadSize) {
        return makeError(FrameErrorCode::PayloadTooLarge, "payload length exceeds configured maximum");
    }

    const auto expectedSize = HeaderSize + static_cast<std::size_t>(payloadLength) + CrcSize;
    if (bytes.size() != expectedSize) {
        std::ostringstream oss;
        oss << "frame length mismatch: expected " << expectedSize << " bytes, got " << bytes.size();
        return makeError(FrameErrorCode::LengthMismatch, oss.str());
    }

    const auto expectedCrc = detail::readU32BE(bytes, bytes.size() - CrcSize);
    const auto calculatedCrc = crc32(bytes.subspan(0U, bytes.size() - CrcSize));
    if (expectedCrc != calculatedCrc) {
        std::ostringstream oss;
        oss << "CRC mismatch: expected 0x" << std::hex << std::setw(8) << std::setfill('0') << expectedCrc
            << ", calculated 0x" << std::setw(8) << calculatedCrc;
        return makeError(FrameErrorCode::BadCrc, oss.str());
    }

    const auto battery = bytes[19U];
    if (battery > 100U) {
        return makeError(FrameErrorCode::InvalidBattery, "battery percent is outside the 0..100 range");
    }

    const auto statusByte = bytes[20U];
    if (statusByte > static_cast<std::uint8_t>(Status::SafeMode)) {
        return makeError(FrameErrorCode::InvalidStatus, "status field contains an unknown value");
    }

    TelemetryFrame frame;
    frame.version = version;
    frame.satelliteId = detail::readU16BE(bytes, 5U);
    frame.timestampMs = detail::readU64BE(bytes, 7U);
    frame.temperatureC = detail::readFloatBE(bytes, 15U);
    frame.batteryPercent = battery;
    frame.status = static_cast<Status>(statusByte);
    frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(HeaderSize),
                         bytes.begin() + static_cast<std::ptrdiff_t>(HeaderSize + payloadLength));

    return frame;
}

const char* errorCodeToString(FrameErrorCode code) noexcept {
    switch (code) {
    case FrameErrorCode::TooShort:
        return "TooShort";
    case FrameErrorCode::BadMagic:
        return "BadMagic";
    case FrameErrorCode::UnsupportedVersion:
        return "UnsupportedVersion";
    case FrameErrorCode::PayloadTooLarge:
        return "PayloadTooLarge";
    case FrameErrorCode::LengthMismatch:
        return "LengthMismatch";
    case FrameErrorCode::BadCrc:
        return "BadCrc";
    case FrameErrorCode::InvalidBattery:
        return "InvalidBattery";
    case FrameErrorCode::InvalidStatus:
        return "InvalidStatus";
    }
    return "Unknown";
}

std::vector<ByteVector> StreamFrameExtractor::feed(std::span<const std::uint8_t> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    std::vector<ByteVector> frames;
    const auto magic = magicBytes();

    while (true) {
        const auto it = std::search(buffer_.begin(), buffer_.end(), magic.begin(), magic.end());
        if (it == buffer_.end()) {
            if (buffer_.size() > MagicSize - 1U) {
                buffer_.erase(buffer_.begin(), buffer_.end() - static_cast<std::ptrdiff_t>(MagicSize - 1U));
            }
            return frames;
        }

        if (it != buffer_.begin()) {
            buffer_.erase(buffer_.begin(), it);
        }

        if (buffer_.size() < HeaderSize) {
            return frames;
        }

        const auto payloadLength = detail::readU16BE(buffer_, 21U);
        if (payloadLength > MaxPayloadSize) {
            buffer_.erase(buffer_.begin());
            continue;
        }

        const auto frameSize = HeaderSize + static_cast<std::size_t>(payloadLength) + CrcSize;
        if (buffer_.size() < frameSize) {
            return frames;
        }

        frames.emplace_back(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
    }
}

void StreamFrameExtractor::clear() {
    buffer_.clear();
}

std::size_t StreamFrameExtractor::bufferedBytes() const noexcept {
    return buffer_.size();
}

} // namespace stgs
