/**
 * @file SignalProcessing.cpp
 * @brief Implémente les filtres FIR et projection sinusoïdale de la démonstration STGS.
 */

#include "stgs/SignalProcessing.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace stgs {
namespace {

constexpr double TwoPi = 2.0 * std::numbers::pi_v<double>;
// Pour un ratio d'amplitudes, la définition du décibel utilise 20*log10(A1/A2).
constexpr double DecibelsPerAmplitudeDecade = 20.0;

struct ProjectionCoefficients {
    double dc = 0.0;
    double sinCoefficient = 0.0;
    double cosCoefficient = 0.0;
};

void validateFiniteSamples(std::span<const float> samples) {
    if (std::any_of(samples.begin(), samples.end(), [](float sample) { return !std::isfinite(sample); })) {
        throw std::invalid_argument("signal processing requires finite samples");
    }
}

void validateProjectionMetadata(std::uint16_t sampleRateHz, float frequencyHz) {
    if (sampleRateHz == 0U || !std::isfinite(frequencyHz)) {
        throw std::invalid_argument("invalid signal metadata for sine projection");
    }
    const float nyquistHz = static_cast<float>(sampleRateHz) / 2.0F;
    if (frequencyHz <= 0.0F || frequencyHz >= nyquistHz) {
        throw std::invalid_argument("sine projection frequency must be strictly between 0 and Nyquist");
    }
}

ProjectionCoefficients estimateProjection(std::span<const float> samples,
                                          std::uint16_t sampleRateHz,
                                          float frequencyHz,
                                          std::uint32_t startSampleIndex) {
    ProjectionCoefficients result;
    if (samples.empty()) {
        return result;
    }
    validateProjectionMetadata(sampleRateHz, frequencyHz);
    validateFiniteSamples(samples);

    double sum = 0.0;
    double sinProjection = 0.0;
    double cosProjection = 0.0;
    const double omega = TwoPi * static_cast<double>(frequencyHz) / static_cast<double>(sampleRateHz);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double x = static_cast<double>(samples[i]);
        const double phase = omega * static_cast<double>(static_cast<std::uint64_t>(startSampleIndex) + i);
        sum += x;
        sinProjection += x * std::sin(phase);
        cosProjection += x * std::cos(phase);
    }

    const double count = static_cast<double>(samples.size());
    result.dc = sum / count;
    // Pour un bloc couvrant plusieurs périodes, sin et cos ont une énergie proche de N/2.
    // Le facteur 2/N transforme donc directement les projections en amplitudes estimées.
    result.sinCoefficient = 2.0 * sinProjection / count;
    result.cosCoefficient = 2.0 * cosProjection / count;
    return result;
}

double rootMeanSquare(std::span<const float> samples) noexcept {
    if (samples.empty()) {
        return 0.0;
    }
    double sumSquares = 0.0;
    for (const float sample : samples) {
        const double x = static_cast<double>(sample);
        sumSquares += x * x;
    }
    return std::sqrt(sumSquares / static_cast<double>(samples.size()));
}

} // namespace

std::vector<float> movingAverageFilter(std::span<const float> samples, std::size_t windowSize) {
    if (windowSize == 0U || (windowSize % 2U) == 0U) {
        throw std::invalid_argument("moving-average window must be a positive odd number");
    }
    validateFiniteSamples(samples);
    if (samples.empty() || windowSize == 1U) {
        return std::vector<float>(samples.begin(), samples.end());
    }

    const std::size_t radius = windowSize / 2U;
    std::vector<float> filtered(samples.size(), 0.0F);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::size_t begin = i > radius ? i - radius : 0U;
        const std::size_t end = std::min(samples.size(), i + radius + 1U);
        double sum = 0.0;
        for (std::size_t j = begin; j < end; ++j) {
            sum += static_cast<double>(samples[j]);
        }
        filtered[i] = static_cast<float>(sum / static_cast<double>(end - begin));
    }
    return filtered;
}

std::vector<float> sineProjectionFilter(std::span<const float> samples,
                                        std::uint16_t sampleRateHz,
                                        float frequencyHz,
                                        std::uint32_t startSampleIndex) {
    const auto coefficients = estimateProjection(samples, sampleRateHz, frequencyHz, startSampleIndex);
    std::vector<float> filtered(samples.size(), 0.0F);
    if (samples.empty()) {
        return filtered;
    }

    const double omega = TwoPi * static_cast<double>(frequencyHz) / static_cast<double>(sampleRateHz);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double phase = omega * static_cast<double>(static_cast<std::uint64_t>(startSampleIndex) + i);
        const double value = coefficients.dc +
                             coefficients.sinCoefficient * std::sin(phase) +
                             coefficients.cosCoefficient * std::cos(phase);
        filtered[i] = static_cast<float>(value);
    }
    return filtered;
}

SignalMetrics computeSignalMetrics(std::span<const float> raw,
                                   std::span<const float> filtered,
                                   std::uint16_t sampleRateHz,
                                   float frequencyHz,
                                   std::uint32_t startSampleIndex) {
    if (raw.size() != filtered.size()) {
        throw std::invalid_argument("raw and filtered signal sizes must match");
    }
    validateProjectionMetadata(sampleRateHz, frequencyHz);
    validateFiniteSamples(raw);
    validateFiniteSamples(filtered);

    SignalMetrics metrics;
    metrics.rawRms = rootMeanSquare(raw);
    metrics.filteredRms = rootMeanSquare(filtered);

    if (!raw.empty()) {
        double residualSquares = 0.0;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const double residual = static_cast<double>(raw[i]) - static_cast<double>(filtered[i]);
            residualSquares += residual * residual;
        }
        metrics.residualNoiseRms = std::sqrt(residualSquares / static_cast<double>(raw.size()));
    }

    const auto coefficients = estimateProjection(raw, sampleRateHz, frequencyHz, startSampleIndex);
    metrics.estimatedAmplitude = std::hypot(coefficients.sinCoefficient, coefficients.cosCoefficient);

    // Seuil purement numérique : évite log10(0) et les divisions instables quand un résidu est
    // nul à la précision double. Il ne représente pas un seuil physique de bruit ou de sensibilité.
    constexpr double NumericalZeroEpsilon = 1.0e-12;
    if (metrics.residualNoiseRms <= NumericalZeroEpsilon) {
        metrics.estimatedSnrDb = std::numeric_limits<double>::infinity();
    } else if (metrics.filteredRms <= NumericalZeroEpsilon) {
        metrics.estimatedSnrDb = -std::numeric_limits<double>::infinity();
    } else {
        metrics.estimatedSnrDb = DecibelsPerAmplitudeDecade *
                                 std::log10(metrics.filteredRms / metrics.residualNoiseRms);
    }
    return metrics;
}

const char* signalFilterModeToString(SignalFilterMode mode) noexcept {
    switch (mode) {
    case SignalFilterMode::None:
        return "none";
    case SignalFilterMode::MovingAverage:
        return "moving-average";
    case SignalFilterMode::SineProjection:
        return "sine-projection";
    }
    return "unknown";
}

} // namespace stgs
