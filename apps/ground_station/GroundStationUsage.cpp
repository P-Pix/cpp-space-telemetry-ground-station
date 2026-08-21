/**
 * @file GroundStationUsage.cpp
 * @brief Contient uniquement l'aide en ligne de commande de la station sol.
 *
 * Séparer l'aide du parseur évite de gonfler le module de validation et permet de faire évoluer
 * la présentation des options sans mélanger logique de contrôle et texte opérateur.
 */
#include "GroundStationOptions.hpp"

#include <iostream>

namespace stgs::app::ground_station
{
  void printUsage()
  {
    std::cout << R"(Space Telemetry Ground Station

Usage:
  stgs_ground_station --udp --port 9000 [options]
  stgs_ground_station --tcp --port 9000 [options]
  stgs_ground_station --tcp --auto-port 9000:9010 [options]
  stgs_ground_station --replay frames.stgf [options]

Input:
  --udp                           Listen for one telemetry frame per UDP datagram.
  --tcp                           Listen for telemetry frames over a TCP byte stream.
  --bind <ipv4>                   Bind address, default 0.0.0.0.
  --port <port>                   Listening port, default 9000.
  --auto-port <a:b>               TCP only: select first locally bindable port in a bounded range.
  --replay <file>                 Replay frames from an STGF capture file.
  --replay-rate <fps>             Replay speed, 0 = as fast as possible.

Pipeline / output:
  --decoder-threads <n>           Parallel decoder workers, default 2.
  --queue-capacity <n>            Backpressure capacity per pipeline queue, default 1024.
  --output <file>                 Output file, default telemetry.csv.
  --output-format <csv|json>      Otherwise inferred from extension.
  --signal-filter <mode>          none, moving-average, sine-projection (default).
  --filter-window <odd-n>         Moving-average FIR window, default 5.

Health monitoring (demo thresholds, configurable):
  --disable-degraded
  --degraded-window <n>           Default 100 samples.
  --degraded-min-samples <n>      Default 20 samples.
  --degraded-rejection-rate <p>   NOMINAL -> DEGRADED, default 0.10.
  --degraded-recovery-rate <p>    DEGRADED -> NOMINAL, default 0.03.
  --degraded-critical-count <n>   Critical count to degrade, default 8.
  --recovery-critical-count <n>   Critical count allowed to recover, default 3.

Diagnostics:
  --log <file>                    Optional log file.
  --log-level <level>             trace, debug, info, warn, error.
  --verbose                       Log each decoded/rejected frame at debug level.
  --no-color                      Disable ANSI colors.
  --help                          Show this help.

Related command:
  stgs_port_check --host 127.0.0.1 --ports 9000:9010
)";
  }

} // namespace stgs::app::ground_station
