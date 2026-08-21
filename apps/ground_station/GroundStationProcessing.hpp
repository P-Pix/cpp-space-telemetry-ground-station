/**
 * @file GroundStationProcessing.hpp
 * @brief Déclare les traitements métier appliqués après décodage d'une trame.
 */
#pragma once

#include "GroundStationOptions.hpp"
#include "stgs/Logger.hpp"
#include "stgs/StationHealth.hpp"
#include "stgs/TelemetryFrame.hpp"
#include "stgs/TerminalUi.hpp"

#include <cstdint>

namespace stgs::app::ground_station
{
    /**
     * @brief Journalise une transition d'état de la station avec le niveau adapté.
     * @param logger Journal applicatif.
     * @param transition Transition calculée par le moniteur de santé.
     */
    void logTransition(Logger &logger, const StationStateTransition &transition);

    /**
     * @brief Interprète un payload STGA et déclenche le rendu opérateur approprié.
     *
     * Un payload historique non-STGA est ignoré silencieusement. Une enveloppe STGA malformée est
     * signalée mais ne transforme pas la TelemetryFrame externe en rejet : framing et CRC étaient
     * valides, seule la couche applicative n'a pas pu être interprétée.
     *
     * @param options Configuration de filtrage du signal.
     * @param frame Trame validée dont le payload doit être inspecté.
     * @param ui Interface terminal utilisée pour les messages et signaux.
     * @param logger Journal utilisé pour les erreurs de couche applicative.
     * @param applicationErrors Compteur incrémenté pour chaque payload STGA invalide.
     */
    void processApplicationPayload(const Options &options,
                                   const TelemetryFrame &frame,
                                   TerminalUi &ui,
                                   Logger &logger,
                                   std::uint64_t &applicationErrors);

} // namespace stgs::app::ground_station
