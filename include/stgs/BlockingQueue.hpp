/**
 * @file BlockingQueue.hpp
 * @brief Définit la file thread-safe du pipeline de télémétrie.
 *
 * Elle relie les étapes réception, décodage et écriture et fournit une sémantique de fermeture pour réveiller les consommateurs à l’arrêt.
 */

#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace stgs {

/**
 * @brief File FIFO thread-safe reliant les étapes du pipeline de station sol.
 *
 * La file n’est pas bornée dans cette implémentation. close() réveille les consommateurs et
 * pop() renvoie std::nullopt seulement lorsque la file fermée ne contient plus d’élément.
 * @tparam T Type transféré entre deux étapes du pipeline.
 */

template <typename T>
class BlockingQueue {
public:
    BlockingQueue() = default;
    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    bool push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return false;
            }
            queue_.push(std::move(value));
        }
        cv_.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop();
        return value;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    bool closed_ = false;
};

} // namespace stgs
