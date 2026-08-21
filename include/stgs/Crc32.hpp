/**
 * @file Crc32.hpp
 * @brief Déclare le CRC-32/ISO-HDLC utilisé pour l'intégrité des trames STGS.
 *
 * Paramètres exacts de la variante : width=32, poly=0x04C11DB7, init=0xFFFFFFFF,
 * refin=true, refout=true et xorout=0xFFFFFFFF. Le vecteur de contrôle standard
 * "123456789" produit 0xCBF43926.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace stgs {

/** Polynôme CRC-32/ISO-HDLC dans sa représentation canonique MSB-first. */
inline constexpr std::uint32_t Crc32Polynomial = 0x04C11DB7U;

/**
 * Polynôme réfléchi utilisé par l'implémentation LSB-first de la table. 0xEDB88320 n'est donc pas
 * un nombre magique : il s'agit du reflet bit à bit de 0x04C11DB7 pour refin=true.
 */
inline constexpr std::uint32_t Crc32ReflectedPolynomial = 0xEDB88320U;
inline constexpr std::uint32_t Crc32InitialValue = 0xFFFFFFFFU;
inline constexpr std::uint32_t Crc32FinalXor = 0xFFFFFFFFU;
inline constexpr std::uint32_t Crc32Check123456789 = 0xCBF43926U;

/**
 * @brief Calcule le CRC-32/ISO-HDLC d'une séquence binaire.
 * @param bytes Octets couverts par le checksum, sans le champ CRC final.
 * @return CRC-32 normalisé selon les paramètres documentés ci-dessus.
 */
std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;

/**
 * @brief Surcharge pratique pour les vecteurs de contrôle textuels.
 * @param text Octets de la chaîne pris tels quels, sans terminateur nul.
 * @return CRC-32/ISO-HDLC de la chaîne.
 */
std::uint32_t crc32(std::string_view text) noexcept;

} // namespace stgs
