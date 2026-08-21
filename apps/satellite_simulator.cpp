/**
 * @file satellite_simulator.cpp
 * @brief Point d'entrée minimal du simulateur de télémétrie STGS.
 *
 * La logique de configuration, génération et transport est répartie dans des modules spécialisés ;
 * `main` ne gère que la frontière processus et la conversion des exceptions en code de sortie.
 */
#include "satellite_simulator/SimulatorApp.hpp"
#include "satellite_simulator/SimulatorOptions.hpp"

#include <exception>
#include <iostream>

int main(int argc, char **argv)
{
    try
    {
        return stgs::app::simulator::run(stgs::app::simulator::parseArgs(argc, argv));
    }
    catch (const std::exception &ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        std::cerr << "run with --help for usage\n";
        return 1;
    }
}
