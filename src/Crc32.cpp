#include "stgs/Crc32.hpp"

#include <array>
#include <cstddef>

namespace stgs {
namespace {

constexpr std::array<std::uint32_t, 256> makeTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
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
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        const auto index = static_cast<std::uint8_t>((crc ^ byte) & 0xFFU);
        crc = (crc >> 8U) ^ CrcTable[index];
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint32_t crc32(std::string_view text) noexcept {
    const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
    return crc32(std::span<const std::uint8_t>(data, text.size()));
}

} // namespace stgs
