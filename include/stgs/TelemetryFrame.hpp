#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace stgs {

using ByteVector = std::vector<std::uint8_t>;

inline constexpr std::uint32_t FrameMagic = 0x53544753U; // ASCII "STGS"
inline constexpr std::uint8_t ProtocolVersion = 1U;
inline constexpr std::size_t MagicSize = 4U;
inline constexpr std::size_t HeaderSize = 23U;
inline constexpr std::size_t CrcSize = 4U;
inline constexpr std::size_t MinFrameSize = HeaderSize + CrcSize;
inline constexpr std::size_t MaxPayloadSize = 4096U;
inline constexpr std::size_t MaxFrameSize = HeaderSize + MaxPayloadSize + CrcSize;

// Binary frame layout, all integer fields are big-endian:
// MAGIC:u32 | VERSION:u8 | SATELLITE_ID:u16 | TIMESTAMP_MS:u64 |
// TEMPERATURE_C:f32 | BATTERY_PERCENT:u8 | STATUS:u8 | PAYLOAD_LEN:u16 |
// PAYLOAD:bytes | CRC32:u32

enum class Status : std::uint8_t {
    Nominal = 0,
    Warning = 1,
    Critical = 2,
    SafeMode = 3
};

struct TelemetryFrame {
    std::uint8_t version = ProtocolVersion;
    std::uint16_t satelliteId = 0;
    std::uint64_t timestampMs = 0;
    float temperatureC = 0.0F;
    std::uint8_t batteryPercent = 0;
    Status status = Status::Nominal;
    ByteVector payload;
};

std::string statusToString(Status status);
std::string payloadToHex(const ByteVector& payload, std::size_t maxBytes = 32U);
std::string toString(const TelemetryFrame& frame);

} // namespace stgs
