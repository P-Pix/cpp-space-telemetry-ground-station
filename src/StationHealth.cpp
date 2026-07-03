#include "stgs/StationHealth.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace stgs {
namespace {

bool isCriticalTelemetry(const TelemetryFrame& frame) noexcept {
    return frame.status == Status::Critical || frame.status == Status::SafeMode;
}

} // namespace

StationHealthMonitor::StationHealthMonitor(StationHealthConfig config)
    : config_(config) {
    if (config_.windowSize == 0U) {
        throw std::invalid_argument("station health window size must be greater than zero");
    }
    if (config_.minSamples == 0U) {
        throw std::invalid_argument("station health minimum sample count must be greater than zero");
    }
    if (config_.minSamples > config_.windowSize) {
        throw std::invalid_argument("station health minimum sample count cannot exceed window size");
    }
    if (config_.degradedRejectionRate < 0.0 || config_.degradedRejectionRate > 1.0) {
        throw std::invalid_argument("degraded rejection rate must be between 0 and 1");
    }
    if (config_.recoveryRejectionRate < 0.0 || config_.recoveryRejectionRate > 1.0) {
        throw std::invalid_argument("recovery rejection rate must be between 0 and 1");
    }
    if (config_.recoveryRejectionRate > config_.degradedRejectionRate) {
        throw std::invalid_argument("recovery rejection rate cannot exceed degraded rejection rate");
    }
}

std::optional<StationStateTransition> StationHealthMonitor::recordDecoded(const TelemetryFrame& frame) {
    return recordSample(Sample{false, isCriticalTelemetry(frame)});
}

std::optional<StationStateTransition> StationHealthMonitor::recordRejected() {
    return recordSample(Sample{true, false});
}

std::optional<StationStateTransition> StationHealthMonitor::recordSample(Sample sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled) {
        return std::nullopt;
    }

    samples_.push_back(sample);
    while (samples_.size() > config_.windowSize) {
        samples_.pop_front();
    }

    const auto snapshot = makeSnapshotLocked();
    if (snapshot.samples < config_.minSamples) {
        return std::nullopt;
    }

    StationState target = state_;
    if (state_ == StationState::Nominal) {
        if (snapshot.rejectionRate >= config_.degradedRejectionRate ||
            snapshot.critical >= config_.criticalFramesForDegraded) {
            target = StationState::Degraded;
        }
    } else {
        if (snapshot.rejectionRate <= config_.recoveryRejectionRate && snapshot.critical == 0U) {
            target = StationState::Nominal;
        }
    }

    if (target == state_) {
        return std::nullopt;
    }

    StationStateTransition transition;
    transition.from = state_;
    transition.to = target;
    transition.snapshot = snapshot;
    transition.reason = transitionReasonLocked(snapshot, target);
    state_ = target;
    transition.snapshot.state = state_;
    return transition;
}

StationHealthSnapshot StationHealthMonitor::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return makeSnapshotLocked();
}

StationState StationHealthMonitor::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

StationHealthSnapshot StationHealthMonitor::makeSnapshotLocked() const {
    StationHealthSnapshot snapshot;
    snapshot.state = state_;
    snapshot.samples = samples_.size();
    for (const auto& sample : samples_) {
        if (sample.rejected) {
            ++snapshot.rejected;
        }
        if (sample.critical) {
            ++snapshot.critical;
        }
    }
    if (snapshot.samples > 0U) {
        snapshot.rejectionRate = static_cast<double>(snapshot.rejected) / static_cast<double>(snapshot.samples);
    }
    return snapshot;
}

std::string StationHealthMonitor::transitionReasonLocked(const StationHealthSnapshot& snapshot,
                                                         StationState target) const {
    std::ostringstream oss;
    if (target == StationState::Degraded) {
        if (snapshot.rejectionRate >= config_.degradedRejectionRate) {
            oss << "rejection rate " << snapshot.rejectionRate
                << " exceeded threshold " << config_.degradedRejectionRate;
        } else {
            oss << "critical telemetry count " << snapshot.critical
                << " reached threshold " << config_.criticalFramesForDegraded;
        }
    } else {
        oss << "rejection rate " << snapshot.rejectionRate
            << " recovered below threshold " << config_.recoveryRejectionRate;
    }
    return oss.str();
}

const char* stationStateToString(StationState state) noexcept {
    switch (state) {
    case StationState::Nominal:
        return "NOMINAL";
    case StationState::Degraded:
        return "DEGRADED";
    }
    return "UNKNOWN";
}

} // namespace stgs
