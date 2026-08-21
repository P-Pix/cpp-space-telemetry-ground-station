/**
 * @file PosixSignalStopController.hpp
 * @brief Déclare le contrôleur d'arrêt propre sur SIGINT/SIGTERM.
 */
#pragma once

#include <atomic>
#include <exception>
#include <mutex>
#include <thread>

#include <signal.h>

namespace stgs::app::ground_station
{

    /**
     * @brief Convertit SIGINT/SIGTERM en demande d'arrêt depuis un thread ordinaire.
     *
     * Objectif projet :
     * éviter un gestionnaire de signal asynchrone qui appellerait des primitives C++ non garanties
     * async-signal-safe. Le thread principal bloque SIGINT/SIGTERM avant la création des workers ; un
     * `std::jthread` dédié les consomme ensuite avec `sigtimedwait()` et met à jour l'atomic `running`.
     *
     * Invariant important :
     * le masque POSIX du thread appelant est toujours restauré, y compris si la construction du thread
     * d'attente échoue ou si une exception remonte depuis le pipeline.
     */
    class PosixSignalStopController
    {
    public:
        /**
         * @brief Bloque SIGINT/SIGTERM et démarre le thread chargé de les attendre.
         * @param running Drapeau partagé pilotant les boucles réseau et replay.
         * @throws std::system_error Si l'installation du masque POSIX échoue.
         */
        explicit PosixSignalStopController(std::atomic_bool &running);

        PosixSignalStopController(const PosixSignalStopController &) = delete;
        PosixSignalStopController &operator=(const PosixSignalStopController &) = delete;

        /** @brief Arrête le thread d'attente et restaure toujours le masque POSIX initial. */
        ~PosixSignalStopController();

        /**
         * @brief Finalise explicitement le contrôleur et relaie une éventuelle erreur d'attente.
         * @throws std::system_error Si `sigtimedwait()` a échoué pour une cause inattendue.
         */
        void finish();

    private:
        void waitLoop(std::stop_token stopToken) noexcept;
        void captureError(std::exception_ptr error) noexcept;
        void stopNoThrow() noexcept;
        void restoreMaskNoThrow() noexcept;

        std::atomic_bool &running_;
        ::sigset_t signalSet_{};
        ::sigset_t previousMask_{};
        bool maskInstalled_ = false;
        std::jthread waiter_;
        std::mutex errorMutex_;
        std::exception_ptr error_;
    };

} // namespace stgs::app::ground_station
