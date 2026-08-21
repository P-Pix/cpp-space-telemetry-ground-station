/**
 * @file GroundStationApp.cpp
 * @brief Assemble les services de haut niveau de la station sol.
 */
#include "GroundStationApp.hpp"

#include "GroundStationPipeline.hpp"
#include "PosixSignalStopController.hpp"
#include "stgs/Logger.hpp"
#include "stgs/PortDiagnostics.hpp"
#include "stgs/StationHealth.hpp"
#include "stgs/TerminalUi.hpp"

#include <atomic>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace stgs::app::ground_station
{

    namespace
    {

        void selectAutomaticPort(Options &options, TerminalUi &ui)
        {
            if (!options.autoPortRange.has_value())
            {
                return;
            }

            ui.section("LOCAL PORT SELECTION");
            std::vector<LocalPortAvailability> report;
            const auto selected = findFirstAvailableLocalPort(
                options.bindAddress, *options.autoPortRange, PortTransport::Tcp, &report);
            for (const auto &entry : report)
            {
                ui.keyValue("port " + std::to_string(entry.port),
                            std::string(entry.available ? "AVAILABLE" : "BUSY/ERROR") + " / " + entry.detail);
            }
            if (!selected.has_value())
            {
                throw std::runtime_error("no bindable port found in --auto-port range");
            }
            options.port = *selected;
            ui.keyValue("selected", std::to_string(options.port));
        }

        void renderStartup(const Options &options, TerminalUi &ui)
        {
            ui.section("GROUND STATION STARTUP");
            if (options.replayFile.has_value())
            {
                ui.keyValue("input", "replay " + options.replayFile->string());
            }
            else
            {
                ui.keyValue("input", transportToString(*options.transport));
                ui.keyValue("listen", options.bindAddress + ":" + std::to_string(options.port));
            }
            ui.keyValue("decoder workers", std::to_string(options.decoderThreads));
            ui.keyValue("queue capacity", std::to_string(options.queueCapacity));
            ui.keyValue("output", options.outputFile.string());
            ui.keyValue("signal filter", signalFilterModeToString(options.signalFilter));
            ui.keyValue("health critical hysteresis",
                        std::to_string(options.healthConfig.criticalFramesForDegraded) + " -> " +
                            std::to_string(options.healthConfig.criticalFramesForRecovery));

            ui.section("PIPELINE");
            ui.keyValue("stage 1", "network/replay -> bounded raw queue");
            ui.keyValue("stage 2", "parallel FrameCodec + CRC validation");
            ui.keyValue("stage 3", "sequence reorder -> deterministic health/export");
            ui.keyValue("stage 4", "STGA message/signal decode -> terminal rendering");
        }

        void renderSummary(const Options &options,
                           const PipelineStats &stats,
                           const StationHealthSnapshot &snapshot,
                           TerminalUi &ui,
                           Logger &logger)
        {
            ui.section("GROUND STATION SUMMARY");
            ui.keyValue("received", std::to_string(stats.received));
            ui.keyValue("decoded", std::to_string(stats.decoded));
            ui.keyValue("rejected", std::to_string(stats.rejected));
            ui.keyValue("written", std::to_string(stats.written));
            ui.keyValue("application errors", std::to_string(stats.applicationErrors));
            ui.keyValue("station state", stationStateToString(snapshot.state));
            ui.keyValue("rejection rate", std::to_string(snapshot.rejectionRate));
            ui.keyValue("output", options.outputFile.string());

            std::ostringstream summary;
            summary << "summary received=" << stats.received
                    << " decoded=" << stats.decoded
                    << " rejected=" << stats.rejected
                    << " written=" << stats.written
                    << " application_errors=" << stats.applicationErrors
                    << " station_state=" << stationStateToString(snapshot.state)
                    << " rejection_rate=" << snapshot.rejectionRate
                    << " output=" << options.outputFile.string();
            logger.info(summary.str());
        }

    } // namespace

    int run(Options options)
    {
        TerminalUi ui(options.color);
        selectAutomaticPort(options, ui);

        Logger logger(options.logFile, options.logLevel);
        StationHealthMonitor health(options.healthConfig);
        std::atomic_bool running{true};
        PosixSignalStopController signalStopController(running);

        renderStartup(options, ui);
        const auto outputFormat = resolveOutputFormat(options.outputFormat, options.outputFile);
        GroundStationPipeline pipeline(options, outputFormat, ui, logger, health, running);
        pipeline.run();

        renderSummary(options, pipeline.stats(), health.snapshot(), ui, logger);
        signalStopController.finish();
        return 0;
    }

} // namespace stgs::app::ground_station
