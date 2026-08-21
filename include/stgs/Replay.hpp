/**
 * @file Replay.hpp
 * @brief Déclare la lecture et l’écriture du format de replay STGF.
 *
 * Le replay conserve les octets complets des trames afin de réutiliser exactement le même chemin de validation que la télémétrie live.
 */

#pragma once

#include "stgs/TelemetryFrame.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <span>

namespace stgs
{

    /**
     * @brief Écrit des trames wire dans un fichier de replay STGF versionné.
     *
     * Chaque trame est préfixée par sa longueur ; les octets sont conservés tels quels, CRC compris,
     * afin que le replay réutilise les validations du chemin live.
     */
    class FrameFileWriter
    {
    public:
        /**
         * @brief Crée/tronque une capture STGF et écrit immédiatement son en-tête versionné.
         * @param path Fichier de destination.
         * @throws std::runtime_error Si le fichier ou son en-tête ne peut pas être écrit.
         */
        explicit FrameFileWriter(const std::filesystem::path &path);

        /** @brief Termine le flux ; les erreurs finales doivent être contrôlées via flush(). */
        ~FrameFileWriter();

        FrameFileWriter(const FrameFileWriter &) = delete;
        FrameFileWriter &operator=(const FrameFileWriter &) = delete;

        /**
         * @brief Ajoute une trame wire au fichier STGF courant.
         * @param frameBytes Trame complète, CRC inclus.
         * @throws std::runtime_error Si la taille dépasse MaxFrameSize ou si l’écriture échoue.
         */
        void writeFrame(std::span<const std::uint8_t> frameBytes);

        /**
         * @brief Force l'écriture des buffers du flux C++ vers le fichier et vérifie son état.
         * @throws std::runtime_error Si le flush révèle une erreur d'E/S.
         */
        void flush();

    private:
        std::ofstream out_;
    };

    /**
     * @brief Lit séquentiellement les trames d’un fichier STGF en contrôlant ses bornes.
     *
     * L’entête de fichier est validé au constructeur et chaque longueur est limitée entre MinFrameSize
     * et MaxFrameSize avant allocation du buffer de trame.
     */

    class FrameFileReader
    {
    public:
        /**
         * @brief Ouvre une capture STGF et valide son magic/version avant toute lecture de trame.
         * @param path Fichier source.
         * @throws std::runtime_error Si le fichier est absent, tronqué ou d'une version inconnue.
         */
        explicit FrameFileReader(const std::filesystem::path &path);

        FrameFileReader(const FrameFileReader &) = delete;
        FrameFileReader &operator=(const FrameFileReader &) = delete;

        /**
         * @brief Lit la prochaine trame wire du replay.
         * @return Trame suivante ou std::nullopt à la fin normale du fichier.
         * @throws std::runtime_error Si le fichier est tronqué ou annonce une longueur interdite.
         */

        std::optional<ByteVector> readNext();

    private:
        std::ifstream in_;
    };

} // namespace stgs
