/**
 * @file SimulatorFrameFactory.cpp
 * @brief Implémente la génération des données de démonstration du satellite simulé.
 */
#include "SimulatorFrameFactory.hpp"

#include "stgs/ApplicationPayload.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace stgs::app::simulator
{
    namespace
    {

        inline constexpr float SimulationTemperatureMeanC = 22.0F;
        inline constexpr float SimulationTemperatureStddevC = 4.0F;

        // Distribution de démonstration : 90 % NOMINAL, 7 % WARNING, 2 % CRITICAL, 1 % SAFE_MODE.
        // Elle exerce la logique de santé et ne prétend modéliser aucune mission réelle.
        inline constexpr std::array<double, 4U> SimulationStatusWeights{90.0, 7.0, 2.0, 1.0};

        // 0x5A = 01011010b : retourne plusieurs bits sans forcer l'octet vers une valeur fixe.
        inline constexpr std::uint8_t CorruptionXorMask = 0x5AU;

        // 101 valeurs produisent la rampe artificielle 100, 99, ..., 0 puis repartent à 100.
        inline constexpr std::size_t SimulationBatteryCycleLength =
            static_cast<std::size_t>(MaxBatteryPercent) + 1U;
        inline constexpr double TwoPi = 2.0 * std::numbers::pi_v<double>;

        std::uint64_t nowMs()
        {
            using namespace std::chrono;
            return static_cast<std::uint64_t>(
                duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
        }

        Status randomStatus(std::mt19937 &rng)
        {
            std::discrete_distribution<int> distribution(
                SimulationStatusWeights.begin(), SimulationStatusWeights.end());
            return static_cast<Status>(distribution(rng));
        }

        ByteVector makeRandomPayload(const Options &options, std::mt19937 &rng)
        {
            std::uniform_int_distribution<unsigned int> byteDistribution(
                0U, std::numeric_limits<std::uint8_t>::max());
            ByteVector payload(options.payloadSize);
            for (auto &byte : payload)
            {
                byte = static_cast<std::uint8_t>(byteDistribution(rng));
            }
            return payload;
        }

        /**
         * @brief Génère un bloc de sinus continu et ajoute éventuellement un bruit blanc gaussien.
         *
         * `blockIndex * signalSamples` fournit l'index absolu du premier échantillon ; la phase ne repart
         * donc pas à zéro à chaque trame. Le bruit est ajouté aux données avant création du payload STGA et
         * avant CRC : il représente une mesure bruitée valide, pas une erreur de transport.
         */
        ByteVector makeSignalPayload(const Options &options, std::mt19937 &rng, std::size_t blockIndex)
        {
            const std::uint64_t startIndex64 = static_cast<std::uint64_t>(blockIndex) *
                                               static_cast<std::uint64_t>(options.signalSamples);
            if (startIndex64 > std::numeric_limits<std::uint32_t>::max())
            {
                throw std::runtime_error("signal sample index exceeds STGA v1 32-bit range");
            }

            std::normal_distribution<float> noise(0.0F, options.noiseStddev);
            SignalBlockPayload signal;
            signal.sampleRateHz = options.sampleRateHz;
            signal.frequencyHz = options.signalFrequencyHz;
            signal.nominalAmplitude = options.signalAmplitude;
            signal.startSampleIndex = static_cast<std::uint32_t>(startIndex64);
            signal.samples.reserve(options.signalSamples);

            const double angularStep = TwoPi * static_cast<double>(options.signalFrequencyHz) /
                                       static_cast<double>(options.sampleRateHz);
            for (std::size_t i = 0U; i < options.signalSamples; ++i)
            {
                const std::uint64_t absoluteIndex = startIndex64 + static_cast<std::uint64_t>(i);
                const double clean = static_cast<double>(options.signalAmplitude) *
                                     std::sin(angularStep * static_cast<double>(absoluteIndex));
                signal.samples.push_back(static_cast<float>(clean) + noise(rng));
            }
            return encodeSignalBlockPayload(signal);
        }

    } // namespace

    TelemetryFrame makeFrame(const Options &options, std::mt19937 &rng, std::size_t sequence)
    {
        std::normal_distribution<float> temperature(
            SimulationTemperatureMeanC, SimulationTemperatureStddevC);

        TelemetryFrame frame;
        frame.satelliteId = options.satelliteId;
        frame.timestampMs = nowMs();
        frame.temperatureC = temperature(rng);
        frame.batteryPercent = static_cast<std::uint8_t>(
            static_cast<std::size_t>(MaxBatteryPercent) - (sequence % SimulationBatteryCycleLength));
        frame.status = randomStatus(rng);

        switch (options.payloadMode)
        {
        case PayloadMode::RandomBytes:
            frame.payload = makeRandomPayload(options, rng);
            break;
        case PayloadMode::TextMessage:
            frame.payload = encodeTextMessagePayload(
                TextMessagePayload{static_cast<std::uint32_t>(sequence), options.message});
            break;
        case PayloadMode::Signal:
            frame.payload = makeSignalPayload(options, rng, sequence);
            break;
        }
        return frame;
    }

    void maybeCorrupt(ByteVector &bytes, std::mt19937 &rng)
    {
        if (bytes.empty())
        {
            return;
        }
        std::uniform_int_distribution<std::size_t> indexDistribution(0U, bytes.size() - 1U);
        bytes[indexDistribution(rng)] ^= CorruptionXorMask;
    }

} // namespace stgs::app::simulator
