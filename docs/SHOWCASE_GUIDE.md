# Running a few STGS scenarios

This is a short set of commands for trying the project on a Linux machine. I usually start with the replay test: it is quick, it does not need open ports, and it gives a useful check of the output ordering.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/stgs_tests
```

The executable reports 20 test cases. The CTest configuration registers them as one test target.

## Replay the same input with different worker counts

```bash
./build/stgs_satellite_simulator --output-file demo.stgf --count 1000 --rate 0 --seed 424242
./build/stgs_ground_station --replay demo.stgf --decoder-threads 1 --output one.csv
./build/stgs_ground_station --replay demo.stgf --decoder-threads 8 --output eight.csv
cmp one.csv eight.csv
```

`cmp` should exit successfully without printing a difference. The decode workers can finish in different orders; the writer stage restores the order in which candidates entered the pipeline. `scripts/verify_demo.sh` also runs the tests and checks JSON syntax if Python 3 is installed.

## Send telemetry over TCP

Terminal A:

```bash
./build/stgs_ground_station --tcp --bind 127.0.0.1 --port 9000 \
  --decoder-threads 4 --output telemetry.csv
```

Terminal B:

```bash
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 \
  --count 1000 --rate 500
```

The receiver stays open until Ctrl+C. To try a text payload instead:

```bash
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 \
  --message "Telemetry link established" --count 1
```

The message is displayed only after the outer STGS frame has passed validation; control characters in received text are escaped before terminal output.

## Generate a noisy signal

Restart the receiver with `--signal-filter sine-projection`, then run:

```bash
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 \
  --signal --sample-rate 200 --signal-frequency 5 \
  --signal-amplitude 1.0 --signal-samples 256 \
  --noise-stddev 0.35 --count 5 --rate 2 --seed 1234
```

The noise is part of otherwise valid frames. The `--corrupt` option is different: it changes encoded bytes after CRC generation, so the frame may be rejected. The sine projection assumes that the nominal frequency is already known; this is a DSP exercise, not an RF receiver.

## Local port checks

```bash
./build/stgs_port_check --host 127.0.0.1 --ports 9000:9010
```

This reports whether TCP listeners respond on the loopback interface. It does **not** identify an STGS service: the protocol has no application-level identity handshake.

## What I would look at in the code

- `src/NetworkServerTcp.cpp`: why TCP needs buffering per connection.
- `src/FrameCodec.cpp`: framing, length checks and CRC.
- `include/stgs/BlockingQueue.hpp`: blocking waits and shutdown behavior.
- `apps/ground_station/GroundStationWorkers.cpp`: deterministic output after parallel decoding.

The current implementation is a demonstration. There is no CCSDS support, authentication, RF demodulation or hard-real-time guarantee. See [the design notes](TECHNICAL_DESIGN.md) and [the dated checks](VERIFICATIONS_2026-11-02.md) for the exact scope.
