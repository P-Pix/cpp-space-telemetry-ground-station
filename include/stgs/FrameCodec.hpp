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

namespace stgs {

enum class FrameErrorCode {
    TooShort,
    BadMagic,
    UnsupportedVersion,
    PayloadTooLarge,
    LengthMismatch,
    BadCrc,
    InvalidBattery,
    InvalidStatus
};

struct FrameError {
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

ByteVector encodeFrame(const TelemetryFrame& frame);
/**
 * @brief Valide une trame reçue et retourne soit la télémétrie, soit une erreur structurée.
 *
 * Les erreurs de données reçues sont des résultats normaux du parseur et ne reposent pas sur
 * des exceptions, ce qui convient au traitement de flux potentiellement bruités.
 * @param bytes Candidat de trame complet.
 * @return TelemetryFrame validée ou FrameError décrivant le rejet.
 */

FrameParseResult decodeFrame(std::span<const std::uint8_t> bytes);
const char* errorCodeToString(FrameErrorCode code) noexcept;

/**
 * @brief Réassemble et resynchronise des trames STGS dans un flux TCP arbitrairement fragmenté.
 *
 * Contrairement à UDP, TCP ne préserve pas les frontières de message. L’extracteur conserve
 * les octets incomplets entre appels et recherche le magic STGS après du bruit ou une trame
 * structurellement impossible.
 */

// Reassembles complete binary telemetry frames from a TCP byte stream.
// UDP does not need this because each datagram is already a candidate frame.
class StreamFrameExtractor {
public:
/**
 * @brief Ajoute un fragment de flux TCP et restitue les trames complètes disponibles.
 * @param bytes Nouveau fragment reçu pour la même connexion TCP.
 * @return Zéro, une ou plusieurs trames wire complètes.
 */

    std::vector<ByteVector> feed(std::span<const std::uint8_t> bytes);
    void clear();
    [[nodiscard]] std::size_t bufferedBytes() const noexcept;

private:
    ByteVector buffer_;
};

} // namespace stgs
