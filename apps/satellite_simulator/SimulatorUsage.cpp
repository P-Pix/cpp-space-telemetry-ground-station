/**
 * @file SimulatorUsage.cpp
 * @brief Contient l'aide de ligne de commande du simulateur STGS.
 */
#include "SimulatorOptions.hpp"

#include <iostream>

namespace stgs::app::simulator
{

  void printUsage()
  {
    std::cout << R"(Satellite Telemetry Simulator

Usage:
  stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 [options]
  stgs_satellite_simulator --tcp --host 127.0.0.1 --discover-ports 9000:9010 [options]
  stgs_satellite_simulator --udp --host 127.0.0.1 --port 9000 [options]
  stgs_satellite_simulator --output-file frames.stgf [options]

Payload modes (mutually exclusive):
  default                         Random binary payload.
  --message <text>                Send typed STGA text messages.
  --signal                        Send typed STGA sine-signal blocks.

Network / replay:
  --udp                           Send one STGS frame per UDP datagram.
  --tcp                           Send frames over a TCP byte stream.
  --host <ipv4>                   Destination host, default 127.0.0.1.
  --port <port>                   Destination port, default 9000.
  --discover-ports <a:b>          TCP loopback only: report ports then use first OPEN listener.
  --discovery-timeout-ms <ms>     Timeout per local TCP probe, default 200.
  --output-file <file>            Also write transmitted wire frames to an STGF replay.

Traffic:
  --count <n>                     Number of frames. Defaults: random=1000, message=1, signal=5.
  --rate <fps>                    Frames/second, 0 = as fast as possible. Default 100.
  --satellite <id>                Satellite ID (0..65535), default 42.
  --payload-size <bytes>          Random mode only, default 32, max 4096.
  --loss <p>                      Simulated drop probability [0,1].
  --corrupt <p>                   Corrupt bytes AFTER CRC with probability [0,1].
  --seed <n>                      Deterministic 32-bit RNG seed.

Signal mode:
  --sample-rate <hz>              Sampling rate, default 200 Hz.
  --signal-frequency <hz>         Sine frequency, default 5 Hz, strictly below Nyquist.
  --signal-amplitude <value>      Nominal amplitude, default 1.0.
  --signal-samples <n>            Samples per STGA block, default 256.
  --noise-stddev <value>          Gaussian noise sigma added BEFORE CRC, default 0.

Display:
  --no-color                      Disable ANSI colors.
  --help                          Show this help.
)";
  }

} // namespace stgs::app::simulator
