/**
 * @file PosixSignalStopController.cpp
 * @brief Implémente l'arrêt coopératif SIGINT/SIGTERM de la station sol.
 */
#include "PosixSignalStopController.hpp"

#include <cerrno>
#include <system_error>
#include <utility>

#include <pthread.h>

namespace stgs::app::ground_station
{
    namespace
    {
        // 100 ms : compromis entre réactivité de Ctrl+C et réveils inutiles du thread d'attente.
        inline constexpr long SignalWaitPollNanoseconds = 100'000'000L;
    } // namespace

    PosixSignalStopController::PosixSignalStopController(std::atomic_bool &running)
        : running_(running)
    {
        if (::sigemptyset(&signalSet_) != 0 ||
            ::sigaddset(&signalSet_, SIGINT) != 0 ||
            ::sigaddset(&signalSet_, SIGTERM) != 0)
        {
            throw std::system_error(errno, std::generic_category(), "failed to build POSIX signal set");
        }

        const int maskResult = ::pthread_sigmask(SIG_BLOCK, &signalSet_, &previousMask_);
        if (maskResult != 0)
        {
            throw std::system_error(maskResult, std::generic_category(), "pthread_sigmask(SIG_BLOCK)");
        }
        maskInstalled_ = true;

        try
        {
            waiter_ = std::jthread([this](std::stop_token stopToken)
                                   { waitLoop(stopToken); });
        }
        catch (...)
        {
            restoreMaskNoThrow();
            throw;
        }
    }

    PosixSignalStopController::~PosixSignalStopController()
    {
        stopNoThrow();
    }

    void PosixSignalStopController::finish()
    {
        stopNoThrow();
        std::lock_guard<std::mutex> lock(errorMutex_);
        if (error_ != nullptr)
        {
            std::rethrow_exception(error_);
        }
    }

    /**
     * @brief Attend les signaux bloqués sans handler asynchrone et avec arrêt coopératif.
     *
     * Algorithme :
     * 1. `sigtimedwait()` attend SIGINT/SIGTERM sur une tranche courte ;
     * 2. un signal attendu bascule immédiatement `running` à false ;
     * 3. EAGAIN signifie seulement que la tranche est écoulée et permet de relire le stop token ;
     * 4. EINTR est retenté ; toute autre erreur est mémorisée puis arrête le pipeline.
     *
     * L'attente temporisée est volontaire : contrairement à `sigwait()`, elle permet à
     * `std::jthread::request_stop()` de terminer le contrôleur même si aucun signal utilisateur arrive.
     */
    void PosixSignalStopController::waitLoop(std::stop_token stopToken) noexcept
    {
        while (!stopToken.stop_requested())
        {
            const ::timespec timeout{0, SignalWaitPollNanoseconds};
            errno = 0;
            const int signalNumber = ::sigtimedwait(&signalSet_, nullptr, &timeout);
            if (signalNumber == SIGINT || signalNumber == SIGTERM)
            {
                running_.store(false);
                return;
            }
            if (signalNumber == -1 && (errno == EAGAIN || errno == EINTR))
            {
                continue;
            }
            if (signalNumber == -1)
            {
                captureError(std::make_exception_ptr(
                    std::system_error(errno, std::generic_category(), "sigtimedwait")));
                running_.store(false);
                return;
            }
        }
    }

    void PosixSignalStopController::captureError(std::exception_ptr error) noexcept
    {
        try
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            if (error_ == nullptr)
            {
                error_ = std::move(error);
            }
        }
        catch (...)
        {
            // L'arrêt reste prioritaire si le mécanisme de mémorisation lui-même devait échouer.
        }
    }

    void PosixSignalStopController::stopNoThrow() noexcept
    {
        if (waiter_.joinable())
        {
            waiter_.request_stop();
            waiter_.join();
        }
        restoreMaskNoThrow();
    }

    void PosixSignalStopController::restoreMaskNoThrow() noexcept
    {
        if (maskInstalled_)
        {
            (void)::pthread_sigmask(SIG_SETMASK, &previousMask_, nullptr);
            maskInstalled_ = false;
        }
    }

} // namespace stgs::app::ground_station
