/**
 * @file SimulatorOptions.hpp
 * @brief Définit la configuration validée du simulateur de télémétrie.
 */
#pragma once

#include "stgs/NetworkServer.hpp"
#include "stgs/PortDiagnostics.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>

namespace stgs::app::simulator
{

    enum class PayloadMode
    {
        RandomBytes,
        TextMessage,
        Signal
    };

    /**
     * @brief Regroupe tous les paramètres de génération et de transport après validation CLI.
     */
    struct Options
    {
        std::optional<Transport> transport;
        std::string host = "127.0.0.1";
        std::uint16_t port = DefaultTelemetryPort;
        std::optional<PortRange> discoverPorts;
        std::chrono::milliseconds discoveryTimeout{DefaultPortProbeTimeout};
        std::size_t count = 1000U;
        bool countExplicit = false;
        double rate = 100.0;
        std::uint16_t satelliteId = 42U;
        std::size_t payloadSize = 32U;
        double lossProbability = 0.0;
        double corruptProbability = 0.0;
        std::uint32_t seed = std::random_device{}();
        std::optional<std::filesystem::path> outputFile;
        PayloadMode payloadMode = PayloadMode::RandomBytes;
        std::string message;
        std::uint16_t sampleRateHz = 200U;
        float signalFrequencyHz = 5.0F;
        float signalAmplitude = 1.0F;
        std::size_t signalSamples = 256U;
        float noiseStddev = 0.0F;
        bool color = true;
    };

    /** @brief Affiche l'aide CLI du simulateur. */
    void printUsage();

    /**
     * @brief Parse la CLI et valide les invariants avant toute transmission ou création de replay.
     * @throws std::runtime_error Si la syntaxe, les bornes ou les combinaisons d'options sont invalides.
     */
    Options parseArgs(int argc, char **argv);

    /** @brief Retourne un libellé stable pour le mode de payload actif. */
    std::string payloadModeName(PayloadMode mode);

} // namespace stgs::app::simulator
