/**
 * @file TerminalUi.hpp
 * @brief Déclare le rendu terminal enrichi utilisé par les outils de démonstration STGS.
 *
 * La couche détecte automatiquement un terminal interactif, respecte NO_COLOR et retombe sur du
 * texte brut lorsque stdout est redirigé. Les messages reçus sont assainis avant affichage afin
 * qu'un payload distant ne puisse pas injecter de séquence de contrôle ANSI dans le terminal.
 */

#pragma once

#include "stgs/SignalProcessing.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace stgs
{

    /** @brief Gravité visuelle d'une ligne de diagnostic opérateur. */
    enum class TerminalStatus
    {
        Info,
        Success,
        Warning,
        Error
    };

    /**
     * @brief Centralise la présentation console des exécutables STGS.
     *
     * Objectif projet :
     * Rendre une démonstration lisible sans dépendance UI externe. Sur un vrai TTY, la classe peut
     * utiliser couleurs ANSI et sparklines Unicode ; en CI, pipe ou fichier, elle produit un texte
     * déterministe sans séquences de contrôle. `std::osyncstream` garde les lignes cohérentes lorsque
     * plusieurs threads écrivent simultanément.
     */
    class TerminalUi
    {
    public:
        /**
         * @brief Détecte les capacités du terminal au moment de la construction.
         * @param allowColor Autorisation applicative ; NO_COLOR et TERM=dumb restent prioritaires.
         */
        explicit TerminalUi(bool allowColor = true);

        /** @brief Indique si stdout est actuellement relié à un terminal interactif. */
        [[nodiscard]] bool interactive() const noexcept { return interactive_; }

        /** @brief Indique si les séquences ANSI de couleur sont effectivement activées. */
        [[nodiscard]] bool colorsEnabled() const noexcept { return colorsEnabled_; }

        /** @brief Retourne la largeur détectée du terminal ou une largeur de repli documentée. */
        [[nodiscard]] std::size_t width() const noexcept;

        /** @brief Affiche un titre de section compact, coloré uniquement sur un TTY compatible. */
        void section(std::string_view title) const;

        /** @brief Affiche une paire clé/valeur utilisée pour rendre configuration et étapes. */
        void keyValue(std::string_view key, std::string_view value) const;

        /**
         * @brief Affiche une ligne de diagnostic structurée avec gravité visuelle.
         *
         * Le rendu utilise un libellé court (`INFO`, `OK`, `WARN`, `ERR`) et une couleur uniquement
         * lorsque stdout est un TTY compatible. La représentation textuelle reste donc exploitable
         * telle quelle dans un log ou un pipeline CI.
         *
         * @param status Niveau visuel du diagnostic.
         * @param subject Élément inspecté, par exemple `port 9000`.
         * @param detail Résultat détaillé destiné à l'opérateur.
         */
        void statusLine(TerminalStatus status,
                        std::string_view subject,
                        std::string_view detail) const;

        /**
         * @brief Affiche un message applicatif après neutralisation des contrôles terminaux.
         * @param satelliteId Source logique de la trame.
         * @param sequence Séquence de la couche message STGA.
         * @param text Texte reçu ; les octets de contrôle sont échappés avant rendu.
         */
        void receivedMessage(std::uint16_t satelliteId,
                             std::uint32_t sequence,
                             std::string_view text) const;

        /**
         * @brief Affiche un bloc de signal sous forme de sparklines brute/filtrée et de métriques.
         *
         * Le nombre de colonnes est adapté à la largeur du terminal et borné pour éviter qu'un bloc
         * de milliers d'échantillons ne rende la sortie illisible.
         *
         * @param satelliteId Source logique du bloc de signal.
         * @param sampleRateHz Fréquence d'échantillonnage annoncée dans STGA.
         * @param frequencyHz Fréquence nominale transportée avec le signal.
         * @param nominalAmplitude Amplitude de référence générée par le simulateur.
         * @param raw Échantillons reçus, incluant l'éventuel bruit de démonstration.
         * @param filtered Échantillons reconstruits par le filtre sélectionné.
         * @param metrics Résumé quantitatif calculé sur raw/filtered.
         * @param filterMode Filtre réellement appliqué, affiché à l'opérateur.
         */
        void receivedSignal(std::uint16_t satelliteId,
                            std::uint16_t sampleRateHz,
                            float frequencyHz,
                            float nominalAmplitude,
                            std::span<const float> raw,
                            std::span<const float> filtered,
                            const SignalMetrics &metrics,
                            SignalFilterMode filterMode) const;

        /**
         * @brief Neutralise les contrôles ASCII/ANSI avant affichage d'un texte reçu du réseau.
         * @return Texte imprimable où ESC, CR, LF, TAB et contrôles C0/DEL sont rendus explicitement.
         */
        static std::string sanitizeRemoteText(std::string_view text);

        /**
         * @brief Compresse un bloc numérique en représentation sparkline.
         *
         * Les échantillons sont regroupés par colonnes puis moyennés. Chaque moyenne est normalisée
         * entre le minimum et le maximum du bloc avant sélection de l'un des huit niveaux visuels.
         * @param samples Signal à représenter.
         * @param columns Nombre maximal de colonnes de sortie.
         * @param unicode true pour les huit blocs Unicode, false pour une palette ASCII portable.
         */
        static std::string sparkline(std::span<const float> samples, std::size_t columns, bool unicode);

    private:
        /** @brief Enveloppe un texte dans un code SGR uniquement lorsque les couleurs sont actives. */
        [[nodiscard]] std::string style(std::string_view text, std::string_view sgrCode) const;

        bool interactive_ = false;
        bool colorsEnabled_ = false;
        bool unicodeEnabled_ = false;
    };

} // namespace stgs
