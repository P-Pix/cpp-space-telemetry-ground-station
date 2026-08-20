/**
 * @file Crc32.hpp
 * @brief Déclare le CRC-32 utilisé pour valider les trames de télémétrie.
 *
 * La station refuse une trame dont le checksum ne correspond pas au contenu reçu.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace stgs {

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;
std::uint32_t crc32(std::string_view text) noexcept;

} // namespace stgs
