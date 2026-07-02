# cpp-space-telemetry-ground-station

A C++20 Linux-based telemetry ground station simulator for receiving, decoding, validating and replaying satellite-like telemetry frames over TCP/UDP.

The project models a small space telemetry ground station. It contains a C++ satellite simulator, a TCP/UDP receiving service, a binary frame decoder, CRC validation, thread-safe logging, a multithreaded processing pipeline, replay files, unit tests and technical documentation.

## Features

- C++20 codebase with clean RAII-based classes and standard-library concurrency.
- Binary telemetry frame codec using `std::variant` for decode results and `std::optional` for replay/queue end-of-stream handling.
- CRC-32 validation using the standard Ethernet/ZIP polynomial.
- UDP mode: one complete frame per datagram.
- TCP mode: stream reassembly with magic-word synchronization.
- Multithreaded ground-station pipeline: receiver thread/main loop, decoder workers and CSV writer thread.
- Replay mode using an `.stgf` binary capture file.
- Satellite simulator with configurable frame rate, payload size, loss probability and corruption probability.
- Unit tests for CRC, parser, bad frames, replay and the blocking queue.
- No third-party runtime dependencies.

## Frame format

All integer fields are big-endian. The CRC covers every byte before the CRC field.

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

- Linux
- CMake 3.16+
- A C++20 compiler such as GCC 10+ or Clang 12+

## Build

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

Replay it through the same decoding and CSV-writing pipeline:

```bash
./build/stgs_ground_station --replay frames.stgf --replay-rate 1000 --output replay.csv
```

You can also record exactly the frames sent by the simulator while it transmits over UDP or TCP:

```bash
./build/stgs_satellite_simulator --udp --host 127.0.0.1 --port 9000 --count 1000 --output-file capture.stgf
```

## Error injection

Simulate packet loss and corrupted frames:

```bash
./build/stgs_satellite_simulator --udp --host 127.0.0.1 --port 9000 \
  --count 5000 --rate 2000 --loss 0.02 --corrupt 0.01
```

Corrupted frames are rejected by the decoder with a CRC error or a structural frame error.

## Useful commands

```bash
./build/stgs_ground_station --help
./build/stgs_satellite_simulator --help
```

## Project structure

```text
.
├── apps/
│   ├── ground_station.cpp          # TCP/UDP service and replay mode
│   └── satellite_simulator.cpp     # C++ telemetry generator
├── docs/
│   └── TECHNICAL_DESIGN.md         # Technical documentation
├── include/stgs/
│   ├── BlockingQueue.hpp
│   ├── ByteUtils.hpp
│   ├── Crc32.hpp
│   ├── FrameCodec.hpp
│   ├── Logger.hpp
│   ├── NetworkServer.hpp
│   ├── Replay.hpp
│   └── TelemetryFrame.hpp
├── src/
│   ├── Crc32.cpp
│   ├── FrameCodec.cpp
│   ├── Logger.cpp
│   ├── NetworkServer.cpp
│   └── Replay.cpp
└── tests/
    └── test_main.cpp
```

## Design notes

The ground station is intentionally split into small libraries:

- `stgs_core`: frame model, binary codec, CRC, logging and replay files.
- `stgs_network`: Linux TCP/UDP receiving service.
- `stgs_ground_station`: CLI application that connects networking/replay with decoder workers and writer thread.
- `stgs_satellite_simulator`: CLI telemetry source for live or replay-file workflows.

This keeps the parser and CRC testable without opening sockets.

## License

MIT License. See `LICENSE`.
