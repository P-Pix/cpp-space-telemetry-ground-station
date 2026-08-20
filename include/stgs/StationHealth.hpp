/**
 * @file StationHealth.hpp
 * @brief Déclare le moniteur de santé opérationnelle de la station sol.
 *
 * Il agrège une fenêtre glissante de trames acceptées/rejetées et applique une hystérésis entre les états NOMINAL et DEGRADED.
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
     * Le seuil de récupération peut être inférieur au seuil de dégradation afin de créer une
     * hystérésis et d’éviter les oscillations d’état sur un flux proche de la limite.
     */
    struct StationHealthConfig
    {
        bool enabled = true;
        std::size_t windowSize = 100;
        std::size_t minSamples = 20;
        double degradedRejectionRate = 0.10;
        double recoveryRejectionRate = 0.03;
        std::size_t criticalFramesForDegraded = 3;
    };

    struct StationHealthSnapshot
    {
        StationState state = StationState::Nominal;
        std::size_t samples = 0;
        std::size_t rejected = 0;
        std::size_t critical = 0;
        double rejectionRate = 0.0;
    };

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
     * Objectif projet :
     * Rendre visible une dégradation persistante sans basculer sur une erreur isolée. Une fenêtre
     * glissante agrège les rejets et télémétries critiques ; des seuils distincts de dégradation et
     * de récupération introduisent une hystérésis.
     */
    class StationHealthMonitor
    {
    public:
        explicit StationHealthMonitor(StationHealthConfig config = {});

        StationHealthMonitor(const StationHealthMonitor &) = delete;
        StationHealthMonitor &operator=(const StationHealthMonitor &) = delete;

        /**
         * @brief Ajoute une télémétrie valide à la fenêtre de santé.
         * @param frame Trame déjà validée par FrameCodec.
         * @return Transition d’état si les seuils provoquent une bascule, sinon std::nullopt.
         */
        std::optional<StationStateTransition> recordDecoded(const TelemetryFrame &frame);

        /**
         * @brief Enregistre le rejet d’un candidat de trame.
         * @return Transition éventuelle vers DEGRADED ou vers NOMINAL.
         */
        std::optional<StationStateTransition> recordRejected();

        [[nodiscard]] StationHealthSnapshot snapshot() const;
        [[nodiscard]] StationState state() const;

    private:
        struct Sample
        {
            bool rejected = false;
            bool critical = false;
        };

        std::optional<StationStateTransition> recordSample(Sample sample);
        [[nodiscard]] StationHealthSnapshot makeSnapshotLocked() const;
        [[nodiscard]] std::string transitionReasonLocked(const StationHealthSnapshot &snapshot,
                                                         StationState target) const;

        StationHealthConfig config_;
        mutable std::mutex mutex_;
        std::deque<Sample> samples_;
        StationState state_ = StationState::Nominal;
    };

    const char *stationStateToString(StationState state) noexcept;

} // namespace stgs
