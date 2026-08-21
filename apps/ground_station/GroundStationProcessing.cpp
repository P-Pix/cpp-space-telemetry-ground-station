/**
 * @file GroundStationProcessing.cpp
 * @brief Implémente l'interprétation STGA et le rendu des transitions de santé.
 */
#include "GroundStationProcessing.hpp"

#include "stgs/ApplicationPayload.hpp"
#include "stgs/SignalProcessing.hpp"

#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace stgs::app::ground_station
{
    void logTransition(Logger &logger, const StationStateTransition &transition)
    {
        std::ostringstream message;
        message << "station state changed: " << stationStateToString(transition.from)
                << " -> " << stationStateToString(transition.to)
                << ", samples=" << transition.snapshot.samples
                << ", rejected=" << transition.snapshot.rejected
                << ", rejection_rate=" << transition.snapshot.rejectionRate
                << ", critical=" << transition.snapshot.critical
                << ", reason=" << transition.reason;
        if (transition.to == StationState::Degraded)
        {
            logger.warning(message.str());
        }
        else
        {
            logger.info(message.str());
        }
    }

    void processApplicationPayload(const Options &options,
                                   const TelemetryFrame &frame,
                                   TerminalUi &ui,
                                   Logger &logger,
                                   std::uint64_t &applicationErrors)
    {
        auto parsed = decodeApplicationPayload(frame.payload);
        if (std::holds_alternative<std::monostate>(parsed))
        {
            return;
        }
        if (const auto *message = std::get_if<TextMessagePayload>(&parsed))
        {
            ui.receivedMessage(frame.satelliteId, message->sequence, message->text);
            return;
        }
        if (const auto *signal = std::get_if<SignalBlockPayload>(&parsed))
        {
            std::vector<float> filtered;
            switch (options.signalFilter)
            {
            case SignalFilterMode::None:
                filtered = signal->samples;
                break;
            case SignalFilterMode::MovingAverage:
                filtered = movingAverageFilter(signal->samples, options.movingAverageWindow);
                break;
            case SignalFilterMode::SineProjection:
                filtered = sineProjectionFilter(signal->samples,
                                                signal->sampleRateHz,
                                                signal->frequencyHz,
                                                signal->startSampleIndex);
                break;
            }

            const auto metrics = computeSignalMetrics(signal->samples,
                                                      filtered,
                                                      signal->sampleRateHz,
                                                      signal->frequencyHz,
                                                      signal->startSampleIndex);
            ui.receivedSignal(frame.satelliteId,
                              signal->sampleRateHz,
                              signal->frequencyHz,
                              signal->nominalAmplitude,
                              signal->samples,
                              filtered,
                              metrics,
                              options.signalFilter);
            return;
        }

        const auto &error = std::get<ApplicationPayloadError>(parsed);
        ++applicationErrors;
        logger.warning(std::string("application payload rejected: ") +
                       applicationPayloadErrorToString(error.code) + " - " + error.message);
    }

} // namespace stgs::app::ground_station
