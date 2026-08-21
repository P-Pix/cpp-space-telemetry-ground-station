/**
 * @file BlockingQueue.hpp
 * @brief Définit la file FIFO thread-safe et optionnellement bornée du pipeline STGS.
 *
 * Elle relie réception, décodage et écriture. Une capacité non nulle applique une backpressure :
 * les producteurs attendent au lieu de faire croître la mémoire sans limite lorsque le consommateur
 * est temporairement plus lent.
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace stgs
{

    /**
     * @brief File FIFO thread-safe avec fermeture explicite et backpressure optionnelle.
     *
     * close() réveille producteurs et consommateurs. pop() renvoie std::nullopt uniquement lorsque
     * la file est fermée et entièrement drainée. Une capacité de 0 conserve le mode non borné,
     * utile pour certains tests/benchmarks ; les exécutables de production utilisent une capacité.
     * @tparam T Type transféré entre deux étapes du pipeline.
     */
    template <typename T>
    class BlockingQueue
    {
    public:
        /** @brief Construit une file ; `capacity == 0` signifie volontairement non bornée. */
        explicit BlockingQueue(std::size_t capacity = 0U) : capacity_(capacity) {}
        BlockingQueue(const BlockingQueue &) = delete;
        BlockingQueue &operator=(const BlockingQueue &) = delete;

        /**
         * @brief Insère une valeur, en attendant de la place lorsque la file est bornée.
         * @return false si close() a été appelé avant que l'élément puisse être inséré.
         */
        bool push(T value)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            producerCv_.wait(lock, [this]
                             { return closed_ || capacity_ == 0U || queue_.size() < capacity_; });
            if (closed_)
            {
                return false;
            }
            queue_.push(std::move(value));
            lock.unlock();
            consumerCv_.notify_one();
            return true;
        }

        /**
         * @brief Retire l'élément FIFO suivant, en attendant tant que la file ouverte est vide.
         * @return Élément suivant, ou std::nullopt lorsque la file fermée est complètement drainée.
         */
        std::optional<T> pop()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            consumerCv_.wait(lock, [this]
                             { return closed_ || !queue_.empty(); });
            if (queue_.empty())
            {
                return std::nullopt;
            }
            T value = std::move(queue_.front());
            queue_.pop();
            lock.unlock();
            producerCv_.notify_one();
            return value;
        }

        /** @brief Ferme la file et réveille tous les threads éventuellement bloqués. */
        void close()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                closed_ = true;
            }
            consumerCv_.notify_all();
            producerCv_.notify_all();
        }

        /** @brief Indique de manière thread-safe si close() a déjà été demandé. */
        [[nodiscard]] bool closed() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return closed_;
        }

        /** @brief Retourne le nombre courant d'éléments sous verrou. */
        [[nodiscard]] std::size_t size() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.size();
        }

        /** @brief Retourne la capacité configurée ; 0 représente le mode non borné. */
        [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    private:
        mutable std::mutex mutex_;
        std::condition_variable consumerCv_;
        std::condition_variable producerCv_;
        std::queue<T> queue_;
        std::size_t capacity_ = 0U;
        bool closed_ = false;
    };

} // namespace stgs
