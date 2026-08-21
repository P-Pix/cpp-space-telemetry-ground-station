/**
 * @file GroundStationInput.cpp
 * @brief Implémente les producteurs réseau et replay du pipeline.
 */
#include "GroundStationPipeline.hpp"

#include "stgs/NetworkServer.hpp"
#include "stgs/Replay.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace stgs::app::ground_station
{
    void GroundStationPipeline::receiveInput()
    {
        ui_.section("RECEPTION");
        if (options_.replayFile.has_value())
        {
            receiveReplay();
        }
        else
        {
            receiveNetwork();
        }
    }

    /**
     * @brief Rejoue un fichier STGF dans le même chemin que les trames réseau.
     *
     * `replayRate == 0` signifie traitement aussi rapide que possible. Sinon l'échéancier est calculé à
     * partir d'un `steady_clock` et incrémenté à chaque trame, ce qui évite d'accumuler le temps propre
     * au décodage dans la cadence demandée.
     */
    void GroundStationPipeline::receiveReplay()
    {
        FrameFileReader reader(*options_.replayFile);
        logger_.info("replay started from " + options_.replayFile->string());

        const auto delay = options_.replayRate > 0.0
                               ? std::chrono::duration<double>(1.0 / options_.replayRate)
                               : std::chrono::duration<double>(0.0);
        auto nextTick = std::chrono::steady_clock::now();
        while (running_.load())
        {
            auto frame = reader.readNext();
            if (!frame.has_value())
            {
                break;
            }
            submit(std::move(*frame));
            if (delay.count() > 0.0)
            {
                nextTick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
                std::this_thread::sleep_until(nextTick);
            }
        }

        logger_.info("replay completed");
        running_.store(false);
    }

    /**
     * @brief Branche le serveur UDP/TCP sur le producteur unique du pipeline.
     *
     * Le serveur ne connaît ni les workers ni l'export. Son callback transmet simplement chaque unité
     * de framing au pipeline, qui lui attribue ensuite la séquence déterministe utilisée par le writer.
     */
    void GroundStationPipeline::receiveNetwork()
    {
        NetworkConfig config;
        config.transport = *options_.transport;
        config.bindAddress = options_.bindAddress;
        config.port = options_.port;

        NetworkServer server(config, logger_);
        server.run([this](ByteVector bytes)
                   { submit(std::move(bytes)); }, running_);
    }

} // namespace stgs::app::ground_station
