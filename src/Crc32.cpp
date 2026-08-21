/**
 * @file Crc32.cpp
 * @brief Implémente le CRC-32 des trames de télémétrie STGS.
 *
 * Le résultat est utilisé à l’émission et au décodage pour détecter les corruptions de transport ou de replay.
 */

#include "stgs/Crc32.hpp"

#include <array>
#include <cstddef>

namespace stgs {
namespace {

// Une table CRC indexée par octet contient 2^8 = 256 entrées ; chaque entrée traite 8 bits.
constexpr std::size_t CrcTableEntryCount = 256U;
constexpr int BitsPerByte = 8;
constexpr std::uint32_t LeastSignificantBitMask = 1U;
constexpr std::uint32_t LowByteMask = 0xFFU;

constexpr std::array<std::uint32_t, CrcTableEntryCount> makeTable() {
    std::array<std::uint32_t, CrcTableEntryCount> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < BitsPerByte; ++bit) {
            if ((crc & LeastSignificantBitMask) != 0U) {
                crc = (crc >> 1U) ^ Crc32ReflectedPolynomial;
            } else {
                crc >>= 1U;
            }
        }
        table[i] = crc;
    }
    return table;
}

constexpr auto CrcTable = makeTable();

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = Crc32InitialValue;
    for (const auto byte : bytes) {
        const auto index = static_cast<std::uint8_t>((crc ^ byte) & LowByteMask);
        crc = (crc >> static_cast<unsigned>(BitsPerByte)) ^ CrcTable[index];
    }
    return crc ^ Crc32FinalXor;
}

std::uint32_t crc32(std::string_view text) noexcept {
    const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
    return crc32(std::span<const std::uint8_t>(data, text.size()));
}

} // namespace stgs
