/**
 * @file TelemetryOutput.hpp
 * @brief Encapsule l'écriture déterministe des trames vers CSV ou JSON.
 */
#pragma once

#include "GroundStationOptions.hpp"
#include "stgs/TelemetryFrame.hpp"

#include <filesystem>
#include <fstream>

namespace stgs::app::ground_station
{

    /**
     * @brief Gère le cycle de vie d'un export de télémétrie.
     *
     * L'objet ouvre le fichier en troncature, écrit l'en-tête adapté au format puis reçoit les trames
     * déjà remises en ordre par le pipeline. La finalisation écrit la fermeture JSON éventuelle et
     * vérifie explicitement le `flush()` afin qu'une erreur disque ne soit jamais silencieuse.
     */
    class TelemetryOutput
    {
    public:
        /**
         * @brief Ouvre le fichier et initialise le format d'export.
         * @param path Fichier de destination.
         * @param format Format effectif, déjà résolu en CSV ou JSON.
         * @throws std::runtime_error Si le fichier ne peut pas être ouvert.
         */
        TelemetryOutput(const std::filesystem::path &path, OutputFormat format);

        /**
         * @brief Écrit une trame validée dans le format sélectionné.
         * @param frame Trame à sérialiser.
         * @throws std::runtime_error Si le flux passe en erreur pendant l'écriture.
         */
        void write(const TelemetryFrame &frame);

        /**
         * @brief Termine le document et force les données vers le flux de sortie.
         * @throws std::runtime_error Si la finalisation ou le flush échoue.
         */
        void finish();

    private:
        static std::string csvEscape(const std::string &value);
        static std::string jsonEscape(const std::string &value);
        void writeCsvHeader();
        void writeFrameCsv(const TelemetryFrame &frame);
        void writeJsonHeader();
        void writeFrameJson(const TelemetryFrame &frame);

        std::ofstream out_;
        OutputFormat format_ = OutputFormat::Csv;
        bool firstJsonFrame_ = true;
        bool finished_ = false;
    };

} // namespace stgs::app::ground_station
