/**
 * @file GroundStationPipeline.cpp
 * @brief Implémente le cycle de vie et la synchronisation globale du pipeline.
 */
#include "GroundStationPipeline.hpp"

#include <stdexcept>
#include <utility>

namespace stgs::app::ground_station
{

    void AsyncFailure::capture(std::exception_ptr error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_ == nullptr)
        {
            error_ = std::move(error);
        }
    }

    void AsyncFailure::rethrowIfPresent() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_ != nullptr)
        {
            std::rethrow_exception(error_);
        }
    }

    GroundStationPipeline::GroundStationPipeline(Options options,
                                                 OutputFormat outputFormat,
                                                 TerminalUi &ui,
                                                 Logger &logger,
                                                 StationHealthMonitor &health,
                                                 std::atomic_bool &running)
        : options_(std::move(options)),
          outputFormat_(outputFormat),
          ui_(ui),
          logger_(logger),
          health_(health),
          running_(running),
          output_(options_.outputFile, outputFormat_),
          rawQueue_(options_.queueCapacity),
          decodedQueue_(options_.queueCapacity)
    {
    }

    /**
     * @brief Exécute le pipeline avec une seule stratégie de nettoyage pour tous les chemins d'erreur.
     *
     * Le point délicat est l'ordre d'arrêt : les producteurs ferment d'abord la file brute, les
     * décodeurs la drainent et sont joints, puis seulement la file décodée est fermée pour permettre au
     * writer de consommer les derniers résultats. En cas d'exception pendant l'entrée, `failPipeline()`
     * ferme immédiatement les deux files pour débloquer tous les threads avant de relayer l'erreur.
     */
    void GroundStationPipeline::run()
    {
        startWorkers();
        try
        {
            receiveInput();
        }
        catch (...)
        {
            failPipeline(std::current_exception());
        }

        stopWorkers();
        asyncFailure_.rethrowIfPresent();
    }

    PipelineStats GroundStationPipeline::stats() const noexcept
    {
        return PipelineStats{
            received_.load(),
            decoded_.load(),
            rejected_.load(),
            written_.load(),
            applicationErrors_.load()};
    }

    void GroundStationPipeline::startWorkers()
    {
        progressThread_ = std::jthread([this](std::stop_token stopToken)
                                       { progressLoop(stopToken); });

        writerThread_ = std::jthread([this]
                                     { writerLoop(); });

        decoderThreads_.reserve(options_.decoderThreads);
        for (std::size_t workerIndex = 0U; workerIndex < options_.decoderThreads; ++workerIndex)
        {
            decoderThreads_.emplace_back([this, workerIndex]
                                         { decoderLoop(workerIndex); });
        }
    }

    void GroundStationPipeline::stopWorkers()
    {
        rawQueue_.close();
        for (auto &thread : decoderThreads_)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }

        decodedQueue_.close();
        if (writerThread_.joinable())
        {
            writerThread_.join();
        }

        if (progressThread_.joinable())
        {
            progressThread_.request_stop();
            progressWakeup_.notify_all();
            progressThread_.join();
        }
    }

    void GroundStationPipeline::failPipeline(std::exception_ptr error) noexcept
    {
        try
        {
            asyncFailure_.capture(std::move(error));
        }
        catch (...)
        {
            // Le pipeline doit malgré tout libérer les threads si la mémorisation de l'erreur échoue.
        }
        running_.store(false);
        rawQueue_.close();
        decodedQueue_.close();
        progressWakeup_.notify_all();
    }

    void GroundStationPipeline::submit(ByteVector frameBytes)
    {
        RawWorkItem work;
        work.sequence = nextSequence_.fetch_add(1U);
        work.bytes = std::move(frameBytes);
        if (!rawQueue_.push(std::move(work)))
        {
            running_.store(false);
            return;
        }
        ++received_;
    }

} // namespace stgs::app::ground_station
