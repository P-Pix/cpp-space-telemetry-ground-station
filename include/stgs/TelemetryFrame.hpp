/**
 * @file TelemetryFrame.hpp
 * @brief Définit le modèle métier, le layout wire et les limites d'une trame STGS v1.
 *
 * Les offsets sont dérivés des tailles des champs afin d'éviter les nombres magiques dans le
 * codec. Un static_assert verrouille la taille du header v1 : toute évolution du protocole doit
 * donc être volontaire et visible en revue de code.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace stgs
{

    using ByteVector = std::vector<std::uint8_t>;

    /** Magic ASCII "STGS" placé au début de chaque trame pour permettre la resynchronisation TCP. */
    inline constexpr std::uint32_t FrameMagic = 0x53544753U;
    inline constexpr std::uint8_t ProtocolVersion = 1U;

    // Tailles wire explicites. Elles ne reposent pas sur sizeof(float) ou sur l'ABI de la machine.
    inline constexpr std::size_t U8WireSize = 1U;
    inline constexpr std::size_t U16WireSize = 2U;
    inline constexpr std::size_t U32WireSize = 4U;
    inline constexpr std::size_t U64WireSize = 8U;
    inline constexpr std::size_t Float32WireSize = 4U;

    inline constexpr std::size_t MagicSize = U32WireSize;
    inline constexpr std::size_t MagicOffset = 0U;
    inline constexpr std::size_t VersionOffset = MagicOffset + U32WireSize;
    inline constexpr std::size_t SatelliteIdOffset = VersionOffset + U8WireSize;
    inline constexpr std::size_t TimestampOffset = SatelliteIdOffset + U16WireSize;
    inline constexpr std::size_t TemperatureOffset = TimestampOffset + U64WireSize;
    inline constexpr std::size_t BatteryOffset = TemperatureOffset + Float32WireSize;
    inline constexpr std::size_t StatusOffset = BatteryOffset + U8WireSize;
    inline constexpr std::size_t PayloadLengthOffset = StatusOffset + U8WireSize;
    inline constexpr std::size_t HeaderSize = PayloadLengthOffset + U16WireSize;
    inline constexpr std::size_t CrcSize = U32WireSize;

    /**
     * Le protocole v1 a un header de 23 octets :
     * 4 + 1 + 2 + 8 + 4 + 1 + 1 + 2. Si ce résultat change, la version du protocole doit être
     * réévaluée plutôt que de modifier silencieusement la disposition binaire.
     */
    static_assert(HeaderSize == 23U, "STGS v1 header size must remain 23 bytes");
    static_assert(sizeof(float) == Float32WireSize, "STGS requires IEEE-like 32-bit float storage");

    inline constexpr std::size_t MinFrameSize = HeaderSize + CrcSize;

    /**
     * Limite applicative volontaire : 4096 octets bornent mémoire, replay et taille d'un datagramme
     * STGS sans prétendre correspondre à une limite physique d'un lien satellite réel.
     */
    inline constexpr std::size_t MaxPayloadSize = 4096U;
    inline constexpr std::size_t MaxFrameSize = HeaderSize + MaxPayloadSize + CrcSize;

    /** Borne métier explicite du champ batterie exprimé en pourcentage entier. */
    inline constexpr std::uint8_t MaxBatteryPercent = 100U;

    /** Nombre d'octets affichés par défaut dans les diagnostics de payload. */
    inline constexpr std::size_t DefaultPayloadPreviewBytes = 32U;

    // Binary frame layout, all integer fields are big-endian:
    // MAGIC:u32 | VERSION:u8 | SATELLITE_ID:u16 | TIMESTAMP_MS:u64 |
    // TEMPERATURE_C:f32 | BATTERY_PERCENT:u8 | STATUS:u8 | PAYLOAD_LEN:u16 |
    // PAYLOAD:bytes | CRC32:u32

    /** @brief État télémétrique transporté sur un octet dans le protocole STGS v1. */
    enum class Status : std::uint8_t
    {
        Nominal = 0U,  ///< Fonctionnement nominal de la source simulée.
        Warning = 1U,  ///< Anomalie non critique à signaler à l'opérateur.
        Critical = 2U, ///< État critique comptabilisé par le moniteur de santé.
        SafeMode = 3U  ///< Mode de sauvegarde, considéré critique pour la santé de station.
    };

    /**
     * @brief Représente une trame de télémétrie validée sous forme typée.
     *
     * Une instance provenant de decodeFrame() a déjà passé les contrôles de structure, de CRC, de
     * batterie et de statut ; le payload reste opaque pour le protocole STGS et peut ensuite être
     * interprété par la couche applicative (message texte, bloc de signal ou données brutes).
     */
    struct TelemetryFrame
    {
        std::uint8_t version = ProtocolVersion;
        std::uint16_t satelliteId = 0;
        std::uint64_t timestampMs = 0;
        float temperatureC = 0.0F;
        std::uint8_t batteryPercent = 0;
        Status status = Status::Nominal;
        ByteVector payload;
    };

    /** @brief Convertit un statut de télémétrie en libellé stable pour logs et exports. */
    std::string statusToString(Status status);

    /**
     * @brief Produit une représentation hexadécimale bornée d'un payload.
     * @param payload Octets applicatifs à afficher.
     * @param maxBytes Nombre maximal d'octets rendus avant troncature visuelle.
     * @return Chaîne hexadécimale lisible par un opérateur.
     */
    std::string payloadToHex(const ByteVector &payload,
                             std::size_t maxBytes = DefaultPayloadPreviewBytes);

    /** @brief Produit un résumé humain d'une trame déjà validée. */
    std::string toString(const TelemetryFrame &frame);

} // namespace stgs
