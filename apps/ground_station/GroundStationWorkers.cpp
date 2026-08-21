/**
 * @file GroundStationWorkers.cpp
 * @brief Implémente les threads de décodage, réordonnancement et observabilité du pipeline.
 */
#include "GroundStationPipeline.hpp"

#include "GroundStationProcessing.hpp"

#include <chrono>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>

namespace stgs::app::ground_station
{
    namespace
    {
        inline constexpr std::chrono::seconds PipelineProgressInterval{1};
    } // namespace

    /**
     * @brief Rend périodiquement l'avancement sans créer de contention avec les workers.
     *
     * La condition variable avec stop token permet un arrêt immédiat. Le mutex du moniteur est relâché
     * avant d'interroger les files et avant le rendu terminal : aucun ordre de verrouillage implicite
     * n'est donc introduit entre le moniteur, les files bornées et `TerminalUi`.
     */
    void GroundStationPipeline::progressLoop(std::stop_token stopToken)
    {
        std::uint64_t previousReceived = 0U;
        std::uint64_t previousDecoded = 0U;
        std::uint64_t previousRejected = 0U;
        std::uint64_t previousWritten = 0U;
        std::unique_lock<std::mutex> lock(progressMutex_);

        while (!stopToken.stop_requested())
        {
            (void)progressWakeup_.wait_for(lock, stopToken, PipelineProgressInterval, []
                                           { return false; });
            if (stopToken.stop_requested())
            {
                break;
            }

            const auto currentReceived = received_.load();
            const auto currentDecoded = decoded_.load();
            const auto currentRejected = rejected_.load();
            const auto currentWritten = written_.load();
            if (currentReceived == previousReceived &&
                currentDecoded == previousDecoded &&
                currentRejected == previousRejected &&
                currentWritten == previousWritten)
            {
                continue;
            }

            lock.unlock();
            std::ostringstream detail;
            detail << "rx=" << currentReceived
                   << " decoded=" << currentDecoded
                   << " rejected=" << currentRejected
                   << " written=" << currentWritten
                   << " queues=" << rawQueue_.size() << '/' << rawQueue_.capacity()
                   << " -> " << decodedQueue_.size() << '/' << decodedQueue_.capacity();
            ui_.statusLine(TerminalStatus::Info, "pipeline", detail.str());
            lock.lock();

            previousReceived = currentReceived;
            previousDecoded = currentDecoded;
            previousRejected = currentRejected;
            previousWritten = currentWritten;
        }
    }

    /**
     * @brief Remet les résultats dans l'ordre de réception avant toute règle métier ou écriture.
     *
     * Plusieurs décodeurs peuvent terminer dans un ordre différent de l'arrivée réseau. Chaque résultat
     * porte donc le numéro attribué avant la file brute. Une `std::map` retient temporairement les trous
     * et le writer ne traite que `expectedSequence`. Santé, messages STGA et export restent ainsi
     * strictement reproductibles avec 1, 4 ou 32 workers.
     *
     * Un résultat manquant à la fermeture est considéré comme une rupture d'invariant : le fichier ne
     * doit jamais être présenté comme complet si une séquence s'est perdue entre les deux files.
     */
    void GroundStationPipeline::writerLoop()
    {
        try
        {
            std::uint64_t expectedSequence = 0U;
            std::map<std::uint64_t, DecodedWorkItem> pending;
            std::uint64_t localApplicationErrors = 0U;

            while (auto incoming = decodedQueue_.pop())
            {
                const auto [iterator, inserted] = pending.emplace(incoming->sequence, std::move(*incoming));
                if (!inserted)
                {
                    throw std::runtime_error("duplicate pipeline sequence detected");
                }
                (void)iterator;

                while (true)
                {
                    auto current = pending.find(expectedSequence);
                    if (current == pending.end())
                    {
                        break;
                    }

                    if (auto *frame = std::get_if<TelemetryFrame>(&current->second.result))
                    {
                        ++decoded_;
                        if (auto transition = health_.recordDecoded(*frame); transition.has_value())
                        {
                            logTransition(logger_, *transition);
                        }
                        processApplicationPayload(options_, *frame, ui_, logger_, localApplicationErrors);
                        output_.write(*frame);
                        ++written_;
                        if (options_.verbose)
                        {
                            logger_.debug("decoded seq=" + std::to_string(expectedSequence) + " " + toString(*frame));
                        }
                    }
                    else
                    {
                        ++rejected_;
                        const auto &error = std::get<FrameError>(current->second.result);
                        if (auto transition = health_.recordRejected(); transition.has_value())
                        {
                            logTransition(logger_, *transition);
                        }
                        logger_.warning("rejected seq=" + std::to_string(expectedSequence) + ": " +
                                        errorCodeToString(error.code) + " - " + error.message);
                    }

                    pending.erase(current);
                    ++expectedSequence;
                }
            }

            if (!pending.empty())
            {
                throw std::runtime_error("pipeline closed with a sequence gap in decoded results");
            }
            applicationErrors_.store(localApplicationErrors);
            output_.finish();
        }
        catch (...)
        {
            failPipeline(std::current_exception());
        }
    }

    /**
     * @brief Décode une file de travail indépendante puis transmet le résultat au writer ordonné.
     *
     * Le worker ne modifie aucun état métier partagé : il ne fait que transformer `ByteVector` en
     * `FrameParseResult`. Cette séparation rend la parallélisation sûre ; les décisions de santé et
     * l'export restent sérialisés dans `writerLoop()`.
     */
    void GroundStationPipeline::decoderLoop(std::size_t workerIndex)
    {
        try
        {
            while (auto work = rawQueue_.pop())
            {
                DecodedWorkItem result;
                result.sequence = work->sequence;
                result.result = decodeFrame(work->bytes);
                if (!decodedQueue_.push(std::move(result)))
                {
                    return;
                }
                if (options_.verbose)
                {
                    logger_.trace("decoder worker=" + std::to_string(workerIndex) +
                                  " completed seq=" + std::to_string(work->sequence));
                }
            }
        }
        catch (...)
        {
            failPipeline(std::current_exception());
        }
    }

} // namespace stgs::app::ground_station
