#pragma once

#include "stgs/TelemetryFrame.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace stgs {

enum class StationState {
    Nominal,
    Degraded
};

struct StationHealthConfig {
    bool enabled = true;
    std::size_t windowSize = 100;
    std::size_t minSamples = 20;
    double degradedRejectionRate = 0.10;
    double recoveryRejectionRate = 0.03;
    std::size_t criticalFramesForDegraded = 3;
};

struct StationHealthSnapshot {
    StationState state = StationState::Nominal;
    std::size_t samples = 0;
    std::size_t rejected = 0;
    std::size_t critical = 0;
    double rejectionRate = 0.0;
};

struct StationStateTransition {
    StationState from = StationState::Nominal;
    StationState to = StationState::Nominal;
    StationHealthSnapshot snapshot;
    std::string reason;
};

class StationHealthMonitor {
public:
    explicit StationHealthMonitor(StationHealthConfig config = {});

    StationHealthMonitor(const StationHealthMonitor&) = delete;
    StationHealthMonitor& operator=(const StationHealthMonitor&) = delete;

    std::optional<StationStateTransition> recordDecoded(const TelemetryFrame& frame);
    std::optional<StationStateTransition> recordRejected();

    [[nodiscard]] StationHealthSnapshot snapshot() const;
    [[nodiscard]] StationState state() const;

private:
    struct Sample {
        bool rejected = false;
        bool critical = false;
    };

    std::optional<StationStateTransition> recordSample(Sample sample);
    [[nodiscard]] StationHealthSnapshot makeSnapshotLocked() const;
    [[nodiscard]] std::string transitionReasonLocked(const StationHealthSnapshot& snapshot,
                                                     StationState target) const;

    StationHealthConfig config_;
    mutable std::mutex mutex_;
    std::deque<Sample> samples_;
    StationState state_ = StationState::Nominal;
};

const char* stationStateToString(StationState state) noexcept;

} // namespace stgs
