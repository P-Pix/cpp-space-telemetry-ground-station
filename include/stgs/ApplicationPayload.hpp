/**
 * @file ApplicationPayload.hpp
 * @brief Définit les payloads applicatifs typés transportés dans une trame STGS v1.
 *
 * Architecture : le wire format STGS n'est pas modifié. Sa zone PAYLOAD peut contenir soit des
 * octets historiques opaques, soit une enveloppe applicative `STGA` versionnée. Cette séparation
 * permet d'ajouter messagerie et signaux sans casser le framing TCP, le CRC, les replays STGF ou les
 * anciens producteurs qui envoient encore un payload arbitraire.
 */

#pragma once

#include "stgs/TelemetryFrame.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace stgs
{

    /** Magic ASCII "STGA" : identifie une enveloppe applicative dans le PAYLOAD STGS. */
    inline constexpr std::uint32_t ApplicationPayloadMagic = 0x53544741U;
    inline constexpr std::uint8_t ApplicationPayloadVersion = 1U;

    // Tailles wire STGA v1 dérivées des primitives STGS. Elles servent aussi à borner la CLI du
    // simulateur : le code appelant n'a donc jamais à recopier "6", "22" ou une capacité calculée.
    inline constexpr std::size_t ApplicationCommonHeaderSize = U32WireSize + U8WireSize + U8WireSize;
    inline constexpr std::size_t TextMessageFixedWireSize = ApplicationCommonHeaderSize + U32WireSize;
    inline constexpr std::size_t SignalBlockFixedWireSize = ApplicationCommonHeaderSize +
                                                            U16WireSize +
                                                            Float32WireSize +
                                                            Float32WireSize +
                                                            U32WireSize +
                                                            U16WireSize;
    inline constexpr std::size_t SignalSampleWireSize = Float32WireSize;
    inline constexpr std::size_t MaxTextMessageBytes = MaxPayloadSize - TextMessageFixedWireSize;
    inline constexpr std::size_t MaxSignalSamplesPerPayload =
        (MaxPayloadSize - SignalBlockFixedWireSize) / SignalSampleWireSize;

    static_assert(ApplicationCommonHeaderSize == 6U, "STGA v1 common header must remain 6 bytes");
    static_assert(SignalBlockFixedWireSize == 22U, "STGA v1 signal header must remain 22 bytes");

    /** @brief Types applicatifs actuellement compris par la station. */
    enum class ApplicationPayloadKind : std::uint8_t
    {
        TextMessage = 1U, ///< Texte UTF-8/opaque accompagné d'un numéro de séquence applicatif.
        SignalBlock = 2U  ///< Métadonnées de signal puis échantillons float32 big-endian.
    };

    /**
     * @brief Message texte transporté dans une trame de télémétrie.
     *
     * `sequence` appartient à la messagerie et reste distinct du numéro interne du pipeline : il
     * permet à un opérateur de suivre plusieurs messages même après capture/replay du flux.
     */
    struct TextMessagePayload
    {
        std::uint32_t sequence = 0U;
        std::string text;
    };

    /**
     * @brief Bloc d'échantillons d'un signal périodique simulé.
     *
     * Objectif projet :
     * Démontrer un petit pipeline DSP reproductible. Le simulateur crée un sinus, peut y ajouter un
     * bruit gaussien avant transport, puis la station reconstruit/filtre ce bloc et affiche des
     * métriques. Il ne s'agit pas d'une modélisation RF ni d'un modem spatial.
     *
     * `startSampleIndex` conserve la phase absolue entre blocs consécutifs ; sans cet index, un
     * filtre corrélatif pourrait introduire une discontinuité à chaque nouvelle trame.
     */
    struct SignalBlockPayload
    {
        std::uint16_t sampleRateHz = 0U;
        float frequencyHz = 0.0F;
        float nominalAmplitude = 0.0F;
        std::uint32_t startSampleIndex = 0U;
        std::vector<float> samples;
    };

    enum class ApplicationPayloadErrorCode
    {
        TruncatedHeader,
        UnsupportedVersion,
        UnknownKind,
        InvalidLength,
        InvalidSignalMetadata
    };

    /** @brief Erreur de parsing applicatif distincte d'une erreur de trame STGS/CRC. */
    struct ApplicationPayloadError
    {
        ApplicationPayloadErrorCode code;
        std::string message;
    };

    /**
     * @brief Résultat de décodage d'un PAYLOAD STGS.
     *
     * `std::monostate` n'est pas une erreur : il signifie simplement que le payload ne commence pas
     * par le magic STGA et doit rester traité comme donnée binaire opaque pour compatibilité.
     */
    using ApplicationPayloadParseResult = std::variant<std::monostate,
                                                       TextMessagePayload,
                                                       SignalBlockPayload,
                                                       ApplicationPayloadError>;

    /**
     * @brief Encode un message texte dans le mini-format applicatif STGA.
     * @param message Séquence logique et contenu à transporter.
     * @return Payload prêt à être affecté à `TelemetryFrame::payload`.
     * @throws std::invalid_argument Si le texte dépasse la capacité d'une trame STGS.
     */
    ByteVector encodeTextMessagePayload(const TextMessagePayload &message);

    /**
     * @brief Encode les métadonnées et échantillons d'un signal dans STGA.
     * @param signal Bloc fini avec sample rate non nul et fréquence strictement sous Nyquist.
     * @return Payload binaire big-endian transportable dans une seule trame.
     * @throws std::invalid_argument Si les métadonnées sont incohérentes ou le bloc trop grand.
     */
    ByteVector encodeSignalBlockPayload(const SignalBlockPayload &signal);

    /**
     * @brief Interprète une enveloppe STGA après validation de la trame externe.
     *
     * La fonction ne recalcule pas le CRC : elle suppose que FrameCodec a déjà validé l'intégrité
     * de la TelemetryFrame. Elle contrôle en revanche version, type, taille et finitude des valeurs
     * propres à la couche applicative, notamment la contrainte 0 < fréquence < Nyquist.
     *
     * @param payload Champ PAYLOAD d'une TelemetryFrame déjà validée.
     * @return Message, signal, erreur structurée ou monostate pour un payload brut non-STGA.
     */
    ApplicationPayloadParseResult decodeApplicationPayload(std::span<const std::uint8_t> payload);

    /** @brief Convertit un type STGA en libellé stable de diagnostic. */
    const char *applicationPayloadKindToString(ApplicationPayloadKind kind) noexcept;

    /** @brief Convertit un code d'erreur STGA en libellé stable de diagnostic. */
    const char *applicationPayloadErrorToString(ApplicationPayloadErrorCode code) noexcept;

} // namespace stgs
