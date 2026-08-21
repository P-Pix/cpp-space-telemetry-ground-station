/**
 * @file SignalProcessing.hpp
 * @brief Déclare les traitements DSP légers de la démonstration de signaux STGS.
 *
 * Les algorithmes sont volontairement lisibles et sans dépendance DSP externe : moyenne glissante
 * pour illustrer un FIR simple, et projection sin/cos lorsque la fréquence nominale est connue.
 * Ils servent de vitrine algorithmique et non de chaîne RF qualifiée.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace stgs
{

    /** @brief Algorithme de reconstruction appliqué aux blocs SIGNAL_BLOCK reçus. */
    enum class SignalFilterMode
    {
        None,          ///< Conserve les échantillons reçus sans transformation.
        MovingAverage, ///< FIR centré simple, utile pour visualiser un lissage générique.
        SineProjection ///< Reconstruction corrélative exploitant la fréquence nominale connue.
    };

    /** @brief Métriques calculées pour comparer signal reçu, reconstruction et résidu. */
    struct SignalMetrics
    {
        double rawRms = 0.0;
        double filteredRms = 0.0;
        double residualNoiseRms = 0.0;
        double estimatedSnrDb = 0.0;
        double estimatedAmplitude = 0.0;
    };

    /**
     * @brief Applique un filtre FIR moyenne glissante centré.
     *
     * Objectif projet :
     * Fournir un filtre temporel simple dont le comportement est immédiatement inspectable. Une
     * fenêtre impaire garantit un nombre identique d'échantillons à gauche et à droite du point
     * courant ; les bords utilisent une fenêtre tronquée plutôt qu'un padding artificiel.
     *
     * @param samples Échantillons bruités.
     * @param windowSize Taille strictement positive et impaire de la fenêtre.
     * @return Signal lissé de même longueur que l'entrée.
     * @throws std::invalid_argument Si la fenêtre est nulle/paire ou si un échantillon n'est pas fini.
     */
    std::vector<float> movingAverageFilter(std::span<const float> samples, std::size_t windowSize);

    /**
     * @brief Reconstruit la composante sinusoïdale connue par projection corrélative.
     *
     * Fonctionnement :
     * chaque échantillon est projeté sur les bases sin(ωn) et cos(ωn), plus une composante continue.
     * Pour un bloc couvrant plusieurs périodes, le bruit large bande est faiblement corrélé à ces
     * bases alors que le sinus utile l'est fortement. Les coefficients obtenus permettent donc de
     * reconstruire une forme d'onde propre sans connaître sa phase initiale.
     *
     * @param samples Bloc reçu, potentiellement bruité.
     * @param sampleRateHz Fréquence d'échantillonnage strictement positive.
     * @param frequencyHz Fréquence nominale du sinus, positive et finie.
     * @param startSampleIndex Index absolu du premier point pour conserver la phase inter-blocs.
     * @return Reconstruction de même longueur que l'entrée.
     * @throws std::invalid_argument Si les échantillons ne sont pas finis ou si la fréquence n'est
     * pas strictement comprise entre 0 et la fréquence de Nyquist.
     */
    std::vector<float> sineProjectionFilter(std::span<const float> samples,
                                            std::uint16_t sampleRateHz,
                                            float frequencyHz,
                                            std::uint32_t startSampleIndex);

    /**
     * @brief Calcule RMS, résidu, amplitude estimée et SNR approximatif.
     *
     * Le bruit est approché par `raw - filtered`. Le SNR est donc une métrique de démonstration
     * dépendante du filtre choisi, pas une mesure instrumentale de puissance RF.
     *
     * @param raw Échantillons tels qu'ils ont été reçus après validation STGA.
     * @param filtered Reconstruction produite par le filtre, de même longueur que raw.
     * @param sampleRateHz Fréquence d'échantillonnage utilisée pour la projection d'amplitude.
     * @param frequencyHz Fréquence nominale du sinus de démonstration.
     * @param startSampleIndex Index absolu permettant de conserver la phase entre blocs.
     * @return Métriques RMS, résidu, SNR approximatif et amplitude fondamentale estimée.
     * @throws std::invalid_argument Si les vecteurs n'ont pas la même taille ou si les métadonnées
     * du signal ne permettent pas le calcul.
     */
    SignalMetrics computeSignalMetrics(std::span<const float> raw,
                                       std::span<const float> filtered,
                                       std::uint16_t sampleRateHz,
                                       float frequencyHz,
                                       std::uint32_t startSampleIndex);

    /** @brief Convertit le filtre en libellé CLI stable. */
    const char *signalFilterModeToString(SignalFilterMode mode) noexcept;

} // namespace stgs
