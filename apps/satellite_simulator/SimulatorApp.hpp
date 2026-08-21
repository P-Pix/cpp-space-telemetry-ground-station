/**
 * @file SimulatorApp.hpp
 * @brief Déclare l'orchestrateur de haut niveau du simulateur STGS.
 */
#pragma once

#include "SimulatorOptions.hpp"

namespace stgs::app::simulator
{

    /**
     * @brief Exécute le scénario de génération et de transmission configuré.
     * @param options Configuration validée issue de la CLI.
     * @return Code processus, 0 en cas de scénario nominal.
     */
    int run(Options options);

} // namespace stgs::app::simulator
