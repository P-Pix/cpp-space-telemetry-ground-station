/**
 * @file SimulatorFrameFactory.hpp
 * @brief Déclare la génération déterministe des trames de démonstration.
 */
#pragma once

#include "SimulatorOptions.hpp"
#include "stgs/TelemetryFrame.hpp"

#include <cstddef>
#include <random>

namespace stgs::app::simulator
{

    /**
     * @brief Construit une trame de télémétrie cohérente pour la séquence demandée.
     * @param options Paramètres de génération validés.
     * @param rng Générateur pseudo-aléatoire partagé du scénario.
     * @param sequence Numéro de trame utilisé pour batterie et payload typé.
     * @return Trame métier prête à être encodée par `FrameCodec`.
     */
    TelemetryFrame makeFrame(const Options &options, std::mt19937 &rng, std::size_t sequence);

    /**
     * @brief Corrompt un octet wire après calcul du CRC afin de simuler une erreur de transport.
     *
     * Le masque XOR fixe retourne plusieurs bits sans imposer une valeur absolue ; seule la position est
     * pseudo-aléatoire. Cette perturbation est volontairement distincte du bruit gaussien du signal,
     * lequel est injecté avant encodage et doit donc conserver un CRC valide.
     */
    void maybeCorrupt(ByteVector &bytes, std::mt19937 &rng);

} // namespace stgs::app::simulator
