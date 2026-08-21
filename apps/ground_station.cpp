/**
 * @file ground_station.cpp
 * @brief Point d'entrée minimal de la station sol STGS.
 *
 * Le parsing et l'orchestration sont volontairement délégués à des modules dédiés afin que `main`
 * reste limité à la frontière processus : convertir la CLI en configuration, lancer l'application
 * puis traduire une exception non gérée en code de sortie et message opérateur.
 */
#include "ground_station/GroundStationApp.hpp"
#include "ground_station/GroundStationOptions.hpp"

#include <exception>
#include <iostream>

int main(int argc, char **argv)
{
    try
    {
        return stgs::app::ground_station::run(stgs::app::ground_station::parseArgs(argc, argv));
    }
    catch (const std::exception &ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        std::cerr << "run with --help for usage\n";
        return 1;
    }
}
