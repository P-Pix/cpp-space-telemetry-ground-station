/**
 * @file Logger.hpp
 * @brief Déclare le logger thread-safe de la station de télémétrie.
 *
 * Le logger fournit un seuil commun aux threads réseau, décodage et écriture. Les logs opérateur
 * restent séparés du rendu enrichi TerminalUi : Logger conserve des lignes timestampées faciles à
 * rediriger vers un fichier ou à analyser automatiquement.
 */

#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace stgs
{

    enum class LogLevel
    {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warning = 3,
        Error = 4
    };

    /**
     * @brief Fournit un flux de diagnostic commun aux threads de la station sol.
     *
     * Les écritures sont sérialisées par mutex afin de préserver une ligne complète par événement.
     * Les niveaux Warning/Error partent sur stderr, les autres sur stdout ; un fichier facultatif
     * reçoit la même représentation timestampée.
     */
    class Logger
    {
    public:
        /**
         * @brief Ouvre éventuellement un fichier de log et configure le seuil initial.
         * @throws std::runtime_error Si le fichier demandé ne peut pas être ouvert.
         */
        explicit Logger(std::optional<std::filesystem::path> filePath = std::nullopt,
                        LogLevel minLevel = LogLevel::Info);
        /** @brief Flush le fichier de log éventuel à la destruction. */
        ~Logger();

        Logger(const Logger &) = delete;
        Logger &operator=(const Logger &) = delete;

        /** @brief Modifie le niveau minimal sous protection du mutex interne. */
        void setMinLevel(LogLevel level);

        /**
         * @brief Émet une ligne timestampée si `level` atteint le seuil courant.
         * @param level Gravité de l'événement.
         * @param message Contenu déjà formaté par l'appelant.
         */
        void log(LogLevel level, std::string_view message);

        /** @brief Journalise un événement très détaillé de diagnostic. */
        void trace(std::string_view message) { log(LogLevel::Trace, message); }
        /** @brief Journalise une information de débogage. */
        void debug(std::string_view message) { log(LogLevel::Debug, message); }
        /** @brief Journalise un événement opérateur normal. */
        void info(std::string_view message) { log(LogLevel::Info, message); }
        /** @brief Journalise une anomalie maîtrisée qui ne stoppe pas nécessairement le service. */
        void warning(std::string_view message) { log(LogLevel::Warning, message); }
        /** @brief Journalise une erreur applicative importante. */
        void error(std::string_view message) { log(LogLevel::Error, message); }

    private:
        std::mutex mutex_;
        std::ofstream file_;
        LogLevel minLevel_;
    };

    /** @brief Convertit un niveau en libellé stable utilisé dans les lignes de log. */
    const char *logLevelToString(LogLevel level) noexcept;

    /** @brief Produit un timestamp UTC ISO-8601 avec précision milliseconde. */
    std::string nowIso8601();

    /** @brief Parse un niveau CLI textuel ; retourne std::nullopt pour une valeur inconnue. */
    std::optional<LogLevel> parseLogLevel(std::string_view value);

} // namespace stgs
