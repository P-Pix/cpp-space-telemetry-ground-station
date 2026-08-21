/**
 * @file ApplicationPayload.cpp
 * @brief Implémente le mini-format applicatif STGA pour messages et signaux.
 */

#include "stgs/ApplicationPayload.hpp"

#include "stgs/ByteUtils.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace stgs
{
    namespace
    {

        constexpr std::size_t AppMagicOffset = 0U;
        constexpr std::size_t AppVersionOffset = AppMagicOffset + U32WireSize;
        constexpr std::size_t AppKindOffset = AppVersionOffset + U8WireSize;

        constexpr std::size_t TextSequenceOffset = ApplicationCommonHeaderSize;
        constexpr std::size_t TextBodyOffset = TextSequenceOffset + U32WireSize;

        constexpr std::size_t SignalSampleRateOffset = ApplicationCommonHeaderSize;
        constexpr std::size_t SignalFrequencyOffset = SignalSampleRateOffset + U16WireSize;
        constexpr std::size_t SignalAmplitudeOffset = SignalFrequencyOffset + Float32WireSize;
        constexpr std::size_t SignalStartIndexOffset = SignalAmplitudeOffset + Float32WireSize;
        constexpr std::size_t SignalSampleCountOffset = SignalStartIndexOffset + U32WireSize;
        constexpr std::size_t SignalSamplesOffset = SignalSampleCountOffset + U16WireSize;

        // Les offsets privés doivent rester cohérents avec les tailles publiques utilisées par la CLI.
        static_assert(TextBodyOffset == TextMessageFixedWireSize,
                      "STGA v1 text header layout changed unexpectedly");
        static_assert(SignalSamplesOffset == SignalBlockFixedWireSize,
                      "STGA v1 signal header layout changed unexpectedly");

        ApplicationPayloadError makeError(ApplicationPayloadErrorCode code, std::string message)
        {
            return ApplicationPayloadError{code, std::move(message)};
        }

        void appendCommonHeader(ByteVector &out, ApplicationPayloadKind kind)
        {
            detail::appendU32BE(out, ApplicationPayloadMagic);
            detail::appendU8(out, ApplicationPayloadVersion);
            detail::appendU8(out, static_cast<std::uint8_t>(kind));
        }

        bool hasApplicationMagic(std::span<const std::uint8_t> payload) noexcept
        {
            return payload.size() >= U32WireSize && detail::readU32BE(payload, AppMagicOffset) == ApplicationPayloadMagic;
        }

        bool validSignalMetadata(std::uint16_t sampleRateHz, float frequencyHz, float nominalAmplitude) noexcept
        {
            if (sampleRateHz == 0U || !std::isfinite(frequencyHz) || !std::isfinite(nominalAmplitude))
            {
                return false;
            }
            const float nyquistHz = static_cast<float>(sampleRateHz) / 2.0F;
            return frequencyHz > 0.0F && frequencyHz < nyquistHz && nominalAmplitude >= 0.0F;
        }

    } // namespace

    ByteVector encodeTextMessagePayload(const TextMessagePayload &message)
    {
        if (message.text.size() > MaxTextMessageBytes)
        {
            throw std::invalid_argument("text message exceeds STGS application payload capacity");
        }

        ByteVector payload;
        payload.reserve(TextBodyOffset + message.text.size());
        appendCommonHeader(payload, ApplicationPayloadKind::TextMessage);
        detail::appendU32BE(payload, message.sequence);
        payload.insert(payload.end(), message.text.begin(), message.text.end());
        return payload;
    }

    ByteVector encodeSignalBlockPayload(const SignalBlockPayload &signal)
    {
        if (!validSignalMetadata(signal.sampleRateHz, signal.frequencyHz, signal.nominalAmplitude))
        {
            throw std::invalid_argument(
                "signal metadata requires finite amplitude, sampleRate > 0 and 0 < frequency < Nyquist");
        }
        if (signal.samples.size() > MaxSignalSamplesPerPayload ||
            signal.samples.size() > std::numeric_limits<std::uint16_t>::max())
        {
            throw std::invalid_argument("signal block contains too many samples for one STGS payload");
        }
        for (const float sample : signal.samples)
        {
            if (!std::isfinite(sample))
            {
                throw std::invalid_argument("signal samples must all be finite");
            }
        }

        ByteVector payload;
        payload.reserve(SignalSamplesOffset + signal.samples.size() * SignalSampleWireSize);
        appendCommonHeader(payload, ApplicationPayloadKind::SignalBlock);
        detail::appendU16BE(payload, signal.sampleRateHz);
        detail::appendFloatBE(payload, signal.frequencyHz);
        detail::appendFloatBE(payload, signal.nominalAmplitude);
        detail::appendU32BE(payload, signal.startSampleIndex);
        detail::appendU16BE(payload, static_cast<std::uint16_t>(signal.samples.size()));
        for (const float sample : signal.samples)
        {
            detail::appendFloatBE(payload, sample);
        }
        return payload;
    }

    /**
     * @brief Décode le mini-protocole STGA sans confondre compatibilité et corruption.
     *
     * Étapes :
     * 1. absence du magic STGA => `monostate`, donc payload historique opaque et valide ;
     * 2. présence du magic => version/type/taille deviennent stricts ;
     * 3. pour SIGNAL_BLOCK, la taille annoncée est vérifiée avant lecture de chaque float ;
     * 4. toutes les métadonnées et tous les échantillons doivent rester finis.
     *
     * Ainsi un préfixe STGA engage explicitement le contrat applicatif, alors qu'un payload ancien
     * continue d'être transporté sans être interprété.
     */
    ApplicationPayloadParseResult decodeApplicationPayload(std::span<const std::uint8_t> payload)
    {
        if (!hasApplicationMagic(payload))
        {
            return std::monostate{};
        }
        if (payload.size() < ApplicationCommonHeaderSize)
        {
            return makeError(ApplicationPayloadErrorCode::TruncatedHeader,
                             "STGA signature is present but the common header is truncated");
        }
        if (payload[AppVersionOffset] != ApplicationPayloadVersion)
        {
            return makeError(ApplicationPayloadErrorCode::UnsupportedVersion,
                             "unsupported STGA application payload version");
        }

        const auto rawKind = payload[AppKindOffset];
        if (rawKind == static_cast<std::uint8_t>(ApplicationPayloadKind::TextMessage))
        {
            if (payload.size() < TextBodyOffset)
            {
                return makeError(ApplicationPayloadErrorCode::InvalidLength,
                                 "text message payload is shorter than its fixed header");
            }
            TextMessagePayload message;
            message.sequence = detail::readU32BE(payload, TextSequenceOffset);
            message.text.assign(reinterpret_cast<const char *>(payload.data() + TextBodyOffset),
                                payload.size() - TextBodyOffset);
            return message;
        }

        if (rawKind == static_cast<std::uint8_t>(ApplicationPayloadKind::SignalBlock))
        {
            if (payload.size() < SignalSamplesOffset)
            {
                return makeError(ApplicationPayloadErrorCode::InvalidLength,
                                 "signal payload is shorter than its fixed header");
            }

            SignalBlockPayload signal;
            signal.sampleRateHz = detail::readU16BE(payload, SignalSampleRateOffset);
            signal.frequencyHz = detail::readFloatBE(payload, SignalFrequencyOffset);
            signal.nominalAmplitude = detail::readFloatBE(payload, SignalAmplitudeOffset);
            signal.startSampleIndex = detail::readU32BE(payload, SignalStartIndexOffset);
            const auto sampleCount = detail::readU16BE(payload, SignalSampleCountOffset);
            const auto expectedSize = SignalSamplesOffset + static_cast<std::size_t>(sampleCount) * SignalSampleWireSize;
            if (payload.size() != expectedSize)
            {
                return makeError(ApplicationPayloadErrorCode::InvalidLength,
                                 "signal sample count does not match the payload length");
            }
            if (!validSignalMetadata(signal.sampleRateHz, signal.frequencyHz, signal.nominalAmplitude))
            {
                return makeError(ApplicationPayloadErrorCode::InvalidSignalMetadata,
                                 "signal metadata requires finite amplitude, sampleRate > 0 and 0 < frequency < Nyquist");
            }

            signal.samples.reserve(sampleCount);
            for (std::size_t i = 0; i < sampleCount; ++i)
            {
                const auto offset = SignalSamplesOffset + i * SignalSampleWireSize;
                const float sample = detail::readFloatBE(payload, offset);
                if (!std::isfinite(sample))
                {
                    return makeError(ApplicationPayloadErrorCode::InvalidSignalMetadata,
                                     "signal payload contains a non-finite sample");
                }
                signal.samples.push_back(sample);
            }
            return signal;
        }

        return makeError(ApplicationPayloadErrorCode::UnknownKind, "unknown STGA application payload kind");
    }

    const char *applicationPayloadKindToString(ApplicationPayloadKind kind) noexcept
    {
        switch (kind)
        {
        case ApplicationPayloadKind::TextMessage:
            return "TEXT_MESSAGE";
        case ApplicationPayloadKind::SignalBlock:
            return "SIGNAL_BLOCK";
        }
        return "UNKNOWN";
    }

    const char *applicationPayloadErrorToString(ApplicationPayloadErrorCode code) noexcept
    {
        switch (code)
        {
        case ApplicationPayloadErrorCode::TruncatedHeader:
            return "TruncatedHeader";
        case ApplicationPayloadErrorCode::UnsupportedVersion:
            return "UnsupportedVersion";
        case ApplicationPayloadErrorCode::UnknownKind:
            return "UnknownKind";
        case ApplicationPayloadErrorCode::InvalidLength:
            return "InvalidLength";
        case ApplicationPayloadErrorCode::InvalidSignalMetadata:
            return "InvalidSignalMetadata";
        }
        return "Unknown";
    }

} // namespace stgs
