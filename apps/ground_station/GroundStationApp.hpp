/**
 * @file GroundStationApp.hpp
 * @brief Déclare l'orchestrateur de haut niveau de la station sol.
 */
#pragma once

#include "GroundStationOptions.hpp"

namespace stgs::app::ground_station
{
    /**
     * @brief Prépare l'environnement opérateur puis exécute le pipeline jusqu'à son arrêt.
     * @param options Configuration validée issue de la CLI.
     * @return Code processus, 0 en cas d'exécution nominale.
     */
    int run(Options options);

} // namespace stgs::app::ground_station
