/**
 * @file ByteUtils.hpp
 * @brief Définit les primitives internes de sérialisation binaire big-endian.
 *
 * Ces helpers constituent la couche la plus basse du protocole STGS et du conteneur STGF. Ils
 * évitent de disperser les décalages de bits et les hypothèses d'endianness dans les codecs.
 * Les fonctions de lecture n'effectuent volontairement aucun contrôle de bornes : elles ne doivent
 * être appelées qu'après validation de la taille par le parseur de niveau supérieur.
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace stgs::detail
{

    // Masque d'un octet (8 bits). Utilisé uniquement pour extraire un byte après décalage.
    inline constexpr std::uint64_t ByteMask = 0xFFU;

    /** @brief Ajoute un octet sans transformation à un buffer wire. */
    inline void appendU8(std::vector<std::uint8_t> &out, std::uint8_t value)
    {
        out.push_back(value);
    }

    /**
     * @brief Sérialise un entier 16 bits en ordre réseau (big-endian).
     * @param out Buffer de destination agrandi de deux octets.
     * @param value Valeur hôte à encoder.
     */
    inline void appendU16BE(std::vector<std::uint8_t> &out, std::uint16_t value)
    {
        out.push_back(static_cast<std::uint8_t>((value >> 8U) & ByteMask));
        out.push_back(static_cast<std::uint8_t>(value & ByteMask));
    }

    /**
     * @brief Sérialise un entier 32 bits en ordre réseau (big-endian).
     *
     * Le masque 0xFF sélectionne explicitement l'octet courant après décalage ; il s'agit d'une
     * opération de sérialisation binaire standard, pas d'une constante métier.
     */
    inline void appendU32BE(std::vector<std::uint8_t> &out, std::uint32_t value)
    {
        out.push_back(static_cast<std::uint8_t>((value >> 24U) & ByteMask));
        out.push_back(static_cast<std::uint8_t>((value >> 16U) & ByteMask));
        out.push_back(static_cast<std::uint8_t>((value >> 8U) & ByteMask));
        out.push_back(static_cast<std::uint8_t>(value & ByteMask));
    }

    /**
     * @brief Sérialise un entier 64 bits en ordre réseau (big-endian).
     *
     * Le décalage initial de 56 bits correspond à l'octet de poids fort d'un entier de 8 octets ;
     * la boucle avance ensuite d'un octet, soit 8 bits, à chaque itération.
     */
    inline void appendU64BE(std::vector<std::uint8_t> &out, std::uint64_t value)
    {
        constexpr int MostSignificantByteShift = 56;
        constexpr int BitsPerByte = 8;
        for (int shift = MostSignificantByteShift; shift >= 0; shift -= BitsPerByte)
        {
            out.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & ByteMask));
        }
    }

    /**
     * @brief Sérialise les bits IEEE-754 d'un float 32 bits en big-endian.
     * @param out Buffer wire de destination.
     * @param value Valeur flottante à conserver bit pour bit.
     */
    inline void appendFloatBE(std::vector<std::uint8_t> &out, float value)
    {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        appendU32BE(out, bits);
    }

    /**
     * @brief Lit un entier 16 bits big-endian depuis un buffer déjà borné.
     * @warning Le caller doit garantir `offset + 2 <= bytes.size()`.
     */
    inline std::uint16_t readU16BE(std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) |
                                          static_cast<std::uint16_t>(bytes[offset + 1U]));
    }

    /**
     * @brief Lit un entier 32 bits big-endian depuis un buffer déjà borné.
     * @warning Le caller doit garantir `offset + 4 <= bytes.size()`.
     */
    inline std::uint32_t readU32BE(std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
               (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
               (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
               static_cast<std::uint32_t>(bytes[offset + 3U]);
    }

    /**
     * @brief Lit un entier 64 bits big-endian depuis un buffer déjà borné.
     * @warning Le caller doit garantir `offset + 8 <= bytes.size()`.
     */
    inline std::uint64_t readU64BE(std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        constexpr std::size_t BytesPerU64 = 8U;
        constexpr unsigned BitsPerByte = 8U;
        std::uint64_t value = 0U;
        for (std::size_t i = 0U; i < BytesPerU64; ++i)
        {
            value = (value << BitsPerByte) | static_cast<std::uint64_t>(bytes[offset + i]);
        }
        return value;
    }

    /**
     * @brief Reconstruit un float 32 bits à partir de son motif binaire big-endian.
     * @warning Le caller doit garantir `offset + 4 <= bytes.size()`.
     */
    inline float readFloatBE(std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        const auto bits = readU32BE(bytes, offset);
        return std::bit_cast<float>(bits);
    }

} // namespace stgs::detail
