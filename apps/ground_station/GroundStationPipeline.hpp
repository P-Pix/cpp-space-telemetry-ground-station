/**
 * @file GroundStationPipeline.hpp
 * @brief Déclare le pipeline concurrent de réception, décodage et export de la station sol.
 */
#pragma once

#include "GroundStationOptions.hpp"
#include "TelemetryOutput.hpp"
#include "stgs/BlockingQueue.hpp"
#include "stgs/FrameCodec.hpp"
#include "stgs/Logger.hpp"
#include "stgs/StationHealth.hpp"
#include "stgs/TerminalUi.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace stgs::app::ground_station
{
    struct RawWorkItem
    {
        std::uint64_t sequence = 0U;
        ByteVector bytes;
    };

    struct DecodedWorkItem
    {
        std::uint64_t sequence = 0U;
        FrameParseResult result;
    };

    /** @brief Compteurs finaux exposés par le pipeline pour le résumé opérateur. */
    struct PipelineStats
    {
        std::uint64_t received = 0U;
        std::uint64_t decoded = 0U;
        std::uint64_t rejected = 0U;
        std::uint64_t written = 0U;
        std::uint64_t applicationErrors = 0U;
    };

    /**
     * @brief Conserve la première exception produite par un worker et la relaie au thread principal.
     *
     * Une exception ne doit jamais sortir directement du point d'entrée d'un `std::jthread`, sous peine
     * d'appeler `std::terminate`. La première erreur est donc mémorisée, les files sont fermées, puis
     * l'exception est relancée après la synchronisation de tous les workers.
     */
    class AsyncFailure
    {
    public:
        void capture(std::exception_ptr error);
        void rethrowIfPresent() const;

    private:
        mutable std::mutex mutex_;
        std::exception_ptr error_;
    };

    /**
     * @brief Orchestre le pipeline borné et déterministe de la station sol.
     *
     * Architecture :
     * 1. réseau ou replay attribue un numéro monotone à chaque trame et alimente `rawQueue_` ;
     * 2. plusieurs workers décodent/valident CRC en parallèle ;
     * 3. le writer remet les résultats dans l'ordre de séquence avant santé, STGA et export ;
     * 4. un moniteur périodique expose compteurs et occupation des files sans perturber le traitement.
     *
     * Les deux files sont bornées afin que la surcharge se traduise par backpressure plutôt que par
     * croissance mémoire illimitée. L'ordre d'export est invariant vis-à-vis du nombre de décodeurs.
     */
    class GroundStationPipeline
    {
    public:
        GroundStationPipeline(Options options,
                              OutputFormat outputFormat,
                              TerminalUi &ui,
                              Logger &logger,
                              StationHealthMonitor &health,
                              std::atomic_bool &running);

        /**
         * @brief Exécute réception, workers et finalisation jusqu'à fin de replay, arrêt ou erreur.
         * @throws std::exception Relaie toute erreur synchrone ou asynchrone du pipeline.
         */
        void run();

        /** @brief Retourne un snapshot atomique des compteurs du pipeline. */
        [[nodiscard]] PipelineStats stats() const noexcept;

    private:
        void startWorkers();
        void stopWorkers();
        void failPipeline(std::exception_ptr error) noexcept;
        void submit(ByteVector frameBytes);

        void progressLoop(std::stop_token stopToken);
        void writerLoop();
        void decoderLoop(std::size_t workerIndex);

        void receiveInput();
        void receiveReplay();
        void receiveNetwork();

        Options options_;
        OutputFormat outputFormat_;
        TerminalUi &ui_;
        Logger &logger_;
        StationHealthMonitor &health_;
        std::atomic_bool &running_;
        TelemetryOutput output_;

        BlockingQueue<RawWorkItem> rawQueue_;
        BlockingQueue<DecodedWorkItem> decodedQueue_;
        std::atomic<std::uint64_t> nextSequence_{0U};
        std::atomic<std::uint64_t> received_{0U};
        std::atomic<std::uint64_t> decoded_{0U};
        std::atomic<std::uint64_t> rejected_{0U};
        std::atomic<std::uint64_t> written_{0U};
        std::atomic<std::uint64_t> applicationErrors_{0U};
        AsyncFailure asyncFailure_;

        std::condition_variable_any progressWakeup_;
        std::mutex progressMutex_;
        std::jthread progressThread_;
        std::jthread writerThread_;
        std::vector<std::jthread> decoderThreads_;
    };

} // namespace stgs::app::ground_station
