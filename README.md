# cpp-space-telemetry-ground-station

![C++ Linux Space Telemetry Ground Station banner](Banniere.png)
*Banner generated with ChatGPT.*

A C++20 Linux-based telemetry ground station simulator for receiving, decoding, validating, replaying and exporting satellite-like telemetry frames over UDP, TCP or STGF replay files.

The project is intentionally designed as an interview-quality systems portfolio project for modern C++ roles involving Linux, binary protocols, telemetry, multithreading, CMake, testing, CI/CD and robust error handling.

## Features

- Configurable satellite telemetry simulator over UDP, TCP or STGF replay files.
- Strict binary protocol with a magic word, protocol revision, bounded payload length and CRC-32 integrity checks.
- Multithreaded producer/consumer decoding pipeline with configurable worker count.
- TCP stream reassembly and resynchronization on malformed or fragmented input.
- Deterministic packet-loss and frame-corruption injection for resilience testing.
- Operational health monitoring with automatic `NOMINAL` and `DEGRADED` state transitions.
- CSV and JSON telemetry export through a shared validated decoding path.
- Thread-safe console and file logging.
- Reproducible decode benchmark and documented reference measurements.
- Dependency-free unit and integration tests driven by CTest.
- Automated Release build, tests, replay smoke checks and benchmark smoke test in GitHub Actions.
- Architecture, protocol, testing and performance documentation.

## Binary protocol

All integer fields use network byte order, also called big-endian.

```text
MAGIC | VERSION | SATELLITE_ID | TIMESTAMP_MS | TEMPERATURE_C |
BATTERY_PERCENT | STATUS | PAYLOAD_LEN | PAYLOAD | CRC32
```

| Field | Type | Description |
| --- | --- | --- |
| `MAGIC` | `uint32` | ASCII `STGS`, value `0x53544753` |
| `VERSION` | `uint8` | Protocol version, currently `1` |
| `SATELLITE_ID` | `uint16` | Simulated spacecraft or equipment ID |
| `TIMESTAMP_MS` | `uint64` | Unix epoch timestamp in milliseconds |
| `TEMPERATURE_C` | `float32` | Equipment temperature in Celsius |
| `BATTERY_PERCENT` | `uint8` | Battery level from `0` to `100` |
| `STATUS` | `uint8` | `0=NOMINAL`, `1=WARNING`, `2=CRITICAL`, `3=SAFE_MODE` |
| `PAYLOAD_LEN` | `uint16` | Payload byte length, maximum `4096` |
| `PAYLOAD` | bytes | Application payload |
| `CRC32` | `uint32` | CRC-32 of all previous bytes |

## Requirements

- Linux.
- CMake 3.16+.
- A C++20 compiler such as GCC 10+ or Clang 12+.
- Python 3 only for the GitHub Actions JSON smoke check.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Quick start: UDP live telemetry

Terminal 1:

```bash
./build/stgs_ground_station --udp --port 9000 --output telemetry.csv --log ground.log
```

Terminal 2:

```bash
./build/stgs_satellite_simulator --udp --host 127.0.0.1 --port 9000 --count 1000 --rate 500
```

Stop the ground station with `Ctrl+C`. Decoded and validated frames are written to `telemetry.csv`.

## Quick start: TCP live telemetry

Terminal 1:

```bash
./build/stgs_ground_station --tcp --port 9000 --decoder-threads 4 --output telemetry.csv
```

Terminal 2:

```bash
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 --count 1000 --rate 500
```

TCP mode reassembles frames from a byte stream and resynchronizes on the `STGS` magic word.

## Replay workflow

Generate a replay file without using the network:

```bash
./build/stgs_satellite_simulator --output-file frames.stgf --count 100 --rate 0
```

Replay it through the same decoding pipeline and export CSV:

```bash
./build/stgs_ground_station --replay frames.stgf --replay-rate 1000 --output replay.csv
```

Replay it and export JSON:

```bash
./build/stgs_ground_station --replay frames.stgf --output replay.json
```

You can also force the output format explicitly:

```bash
./build/stgs_ground_station --replay frames.stgf --output telemetry.out --output-format json
```

## Error injection

Simulate packet loss and corrupted frames:

```bash
./build/stgs_satellite_simulator --udp --host 127.0.0.1 --port 9000 \
  --count 5000 --rate 2000 --loss 0.02 --corrupt 0.01
```

Corrupted frames are rejected by the decoder with a CRC error or a structural frame error.

## Degraded mode

The ground station includes an operational degraded mode. It monitors a sliding window of decoded and rejected frames. The station enters `DEGRADED` when the rejection rate is too high or when too many decoded frames carry critical telemetry. It recovers to `NOMINAL` when the window becomes healthy again.

Example:

```bash
./build/stgs_ground_station --replay corrupt.stgf \
  --output corrupt.json \
  --degraded-window 10 \
  --degraded-min-samples 4 \
  --degraded-rejection-rate 0.25 \
  --degraded-recovery-rate 0.00
```

Typical log:

```text
station state changed: NOMINAL -> DEGRADED, samples=4, rejected=2, rejection_rate=0.5, critical=0
```

## Benchmark

Build and run a reproducible decode benchmark:

```bash
./build/stgs_benchmark_decode --frames 200000 --payload-size 64 --decoder-threads 4
```

Or use the helper script:

```bash
./benchmarks/run_benchmark.sh
```

See [`docs/PERFORMANCE_REPORT.md`](docs/PERFORMANCE_REPORT.md) for benchmark methodology and reference measurements.

## Useful commands

```bash
./build/stgs_ground_station --help
./build/stgs_satellite_simulator --help
./build/stgs_benchmark_decode --help
```

## Project structure

```text
.
├── .github/workflows/
│   └── ci.yml                         # Build, tests, smoke replay and benchmark smoke
├── apps/
│   ├── ground_station.cpp             # TCP/UDP service, replay mode, CSV/JSON export, degraded mode
│   └── satellite_simulator.cpp        # C++ telemetry generator with loss/corruption simulation
├── benchmarks/
│   ├── benchmark_decode.cpp           # Reproducible decode pipeline benchmark
│   └── run_benchmark.sh               # Benchmark helper script
├── docs/
│   ├── PERFORMANCE_REPORT.md          # Performance methodology and reference results
│   └── TECHNICAL_DESIGN.md            # Technical documentation
├── include/stgs/
│   ├── BlockingQueue.hpp
│   ├── ByteUtils.hpp
│   ├── Crc32.hpp
│   ├── FrameCodec.hpp
│   ├── Logger.hpp
│   ├── NetworkServer.hpp
│   ├── Replay.hpp
│   ├── StationHealth.hpp
│   └── TelemetryFrame.hpp
├── src/
│   ├── Crc32.cpp
│   ├── FrameCodec.cpp
│   ├── Logger.cpp
│   ├── NetworkServer.cpp
│   ├── Replay.cpp
│   └── StationHealth.cpp
└── tests/
    └── test_main.cpp
```

## Skills demonstrated

- Modern C++20.
- Linux/POSIX sockets.
- UDP and TCP networking.
- Binary protocol design.
- CRC-32 validation.
- Multithreading with producer/consumer queues.
- CMake and CTest.
- CI/CD with GitHub Actions.
- CSV and JSON export.
- Replay files and deterministic simulations.
- Degraded operational mode.
- Technical documentation in English.

## License

MIT License. See `LICENSE`.
