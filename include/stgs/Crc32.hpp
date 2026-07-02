#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace stgs {

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;
std::uint32_t crc32(std::string_view text) noexcept;

} // namespace stgs
