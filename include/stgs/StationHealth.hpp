/**
 * @file StationHealth.hpp
 * @brief Déclare le moniteur de santé opérationnelle de la station sol.
 *
 * Le moniteur agrège une fenêtre glissante de trames valides/rejetées et applique deux hystérésis :
 * une sur le taux de rejet et une sur le nombre de télémétries critiques. Les seuils par défaut sont
 * des valeurs de démonstration configurables, pas des limites issues d'une norme spatiale.
 */

#pragma once

#include "stgs/TelemetryFrame.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace stgs
{

    enum class StationState
    {
        Nominal,
        Degraded
    };

    /**
     * @brief Paramètre la fenêtre glissante et les seuils de transition de santé.
     *
     * Par défaut, huit télémétries critiques dans une fenêtre de 100 échantillons provoquent DEGRADED,
     * alors que le retour NOMINAL exige au plus trois critiques. Cette hystérésis est volontairement
     * plus large que la distribution de démonstration (~3 % CRITICAL/SAFE_MODE) afin d'éviter le
     * clignotement d'état observé autour d'un seuil égal à la moyenne simulée.
     */
    struct StationHealthConfig
    {
        bool enabled = true;
        std::size_t windowSize = 100U;
        std::size_t minSamples = 20U;
        double degradedRejectionRate = 0.10;
        double recoveryRejectionRate = 0.03;
        std::size_t criticalFramesForDegraded = 8U;
        std::size_t criticalFramesForRecovery = 3U;
    };

    /** @brief Photographie cohérente de la fenêtre de santé au moment de la lecture. */
    struct StationHealthSnapshot
    {
        StationState state = StationState::Nominal;
        std::size_t samples = 0U;
        std::size_t rejected = 0U;
        std::size_t critical = 0U;
        double rejectionRate = 0.0;
    };

    /** @brief Décrit une bascule de santé et la cause qui l'a déclenchée. */
    struct StationStateTransition
    {
        StationState from = StationState::Nominal;
        StationState to = StationState::Nominal;
        StationHealthSnapshot snapshot;
        std::string reason;
    };

    /**
     * @brief Suit la qualité récente du flux et signale les transitions NOMINAL/DEGRADED.
     *
     * La classe est thread-safe, mais l'application principale choisit volontairement de la mettre
     * à jour dans l'ordre de réception après réordonnancement des résultats de décodage. Ainsi le
     * nombre de workers n'influence jamais l'historique de santé.
     */
    class StationHealthMonitor
    {
    public:
        /**
         * @brief Construit le moniteur après validation de toutes les relations entre seuils.
         * @param config Fenêtre, minimum d'observations et deux paires de seuils d'hystérésis.
         * @throws std::invalid_argument Si un taux sort de [0,1], si minSamples dépasse la fenêtre,
         * si le seuil critique est inatteignable dans la fenêtre ou si la récupération n'est pas
         * strictement inférieure au seuil critique de dégradation.
         */
        explicit StationHealthMonitor(StationHealthConfig config = {});

        StationHealthMonitor(const StationHealthMonitor &) = delete;
        StationHealthMonitor &operator=(const StationHealthMonitor &) = delete;

        /**
         * @brief Ajoute une télémétrie valide à la fenêtre de santé.
         * @param frame Trame déjà validée par FrameCodec.
         * @return Transition si les seuils provoquent une bascule, sinon std::nullopt.
         */
        std::optional<StationStateTransition> recordDecoded(const TelemetryFrame &frame);

        /**
         * @brief Enregistre le rejet d'un candidat de trame.
         * @return Transition éventuelle après mise à jour de la fenêtre glissante.
         */
        std::optional<StationStateTransition> recordRejected();

        /** @brief Retourne une photographie thread-safe de la fenêtre courante. */
        [[nodiscard]] StationHealthSnapshot snapshot() const;

        /** @brief Retourne uniquement l'état NOMINAL/DEGRADED courant. */
        [[nodiscard]] StationState state() const;

    private:
        struct Sample
        {
            bool rejected = false;
            bool critical = false;
        };

        /** @brief Insère un échantillon, tronque la fenêtre et évalue une éventuelle transition. */
        std::optional<StationStateTransition> recordSample(Sample sample);

        /** @brief Construit un snapshot ; mutex_ doit déjà être détenu par l'appelant. */
        [[nodiscard]] StationHealthSnapshot makeSnapshotLocked() const;

        /** @brief Explique la règle précise ayant conduit à une transition ; mutex_ doit être détenu. */
        [[nodiscard]] std::string transitionReasonLocked(const StationHealthSnapshot &snapshot,
                                                         StationState target) const;

        StationHealthConfig config_;
        mutable std::mutex mutex_;
        std::deque<Sample> samples_;
        StationState state_ = StationState::Nominal;
    };

    /** @brief Convertit l'état de santé en libellé stable de log/export. */
    const char *stationStateToString(StationState state) noexcept;

} // namespace stgs
