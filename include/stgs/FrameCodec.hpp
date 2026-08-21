/**
 * @file FrameCodec.hpp
 * @brief Déclare le codec strict des trames STGS et le réassembleur de flux TCP.
 *
 * Le décodage retourne une erreur structurée pour les corruptions attendues ; l’extracteur reconstitue les trames à partir d’un flux pouvant être fragmenté ou bruité.
 */

#pragma once

#include "stgs/TelemetryFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace stgs
{

    /** @brief Causes de rejet attendues lors du décodage d'une trame reçue. */
    enum class FrameErrorCode
    {
        TooShort,            ///< La trame ne contient même pas le minimum wire STGS.
        BadMagic,            ///< Le préfixe de resynchronisation n'est pas "STGS".
        UnsupportedVersion,  ///< La version annoncée n'est pas comprise par ce codec.
        PayloadTooLarge,     ///< PAYLOAD_LEN dépasse la limite applicative MaxPayloadSize.
        LengthMismatch,      ///< La taille réelle ne correspond pas au header + payload + CRC.
        BadCrc,              ///< L'intégrité CRC-32/ISO-HDLC ne correspond pas aux octets reçus.
        InvalidTemperature,  ///< La température encodée vaut NaN ou +/-Inf.
        InvalidBattery,      ///< Le pourcentage batterie dépasse 100.
        InvalidStatus        ///< L'octet d'état n'appartient pas à l'enum Status v1.
    };

    /** @brief Diagnostic structuré retourné pour une corruption de données attendue. */
    struct FrameError
    {
        FrameErrorCode code;
        std::string message;
    };

    using FrameParseResult = std::variant<TelemetryFrame, FrameError>;

    /**
     * @brief Sérialise une télémétrie selon le format binaire STGS.
     * @param frame Données typées à encoder.
     * @return Trame complète avec CRC en ordre réseau.
     * @throws std::exception Si un invariant d’encodage, notamment la taille du payload, est invalide.
     */
    ByteVector encodeFrame(const TelemetryFrame &frame);

    /**
     * @brief Valide une trame reçue et retourne soit la télémétrie, soit une erreur structurée.
     *
     * Les erreurs de données reçues sont des résultats normaux du parseur et ne reposent pas sur
     * des exceptions, ce qui convient au traitement de flux potentiellement bruités.
     * @param bytes Candidat de trame complet.
     * @return TelemetryFrame validée ou FrameError décrivant le rejet.
     */
    FrameParseResult decodeFrame(std::span<const std::uint8_t> bytes);

    /** @brief Convertit un code de rejet de trame en libellé stable de diagnostic. */
    const char *errorCodeToString(FrameErrorCode code) noexcept;

    /**
     * @brief Réassemble et resynchronise des trames STGS dans un flux TCP arbitrairement fragmenté.
     *
     * Contrairement à UDP, TCP ne préserve pas les frontières de message. L’extracteur conserve
     * les octets incomplets entre appels et recherche le magic STGS après du bruit ou une trame
     * structurellement impossible.
     */

    class StreamFrameExtractor
    {
    public:
        /**
         * @brief Ajoute un fragment de flux TCP et restitue les trames complètes disponibles.
         * @param bytes Nouveau fragment reçu pour la même connexion TCP.
         * @return Zéro, une ou plusieurs trames wire complètes.
         */
        std::vector<ByteVector> feed(std::span<const std::uint8_t> bytes);
        /** @brief Oublie tout fragment TCP incomplet actuellement mémorisé. */
        void clear();

        /** @brief Retourne le nombre d'octets incomplets conservés entre deux feed(). */
        [[nodiscard]] std::size_t bufferedBytes() const noexcept;

    private:
        ByteVector buffer_;
    };

} // namespace stgs
