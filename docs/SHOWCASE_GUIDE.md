# STGS Showcase Guide

This short guide gives a repeatable interview demonstration of the project without requiring external services.

## 1. Build confidence first

```bash
make test
make asan-test
make tsan-test
```

What this demonstrates: strict compilation, unit/integration tests, real loopback sockets and memory/undefined-behavior instrumentation.

## 2. Start a station without manually choosing a port

Terminal A:

```bash
./build/stgs_ground_station \
  --tcp \
  --bind 127.0.0.1 \
  --auto-port 9000:9010 \
  --decoder-threads 4 \
  --output telemetry.csv
```

Point out the startup sections: selected port, queue capacity, worker count, signal filter and deterministic pipeline stages.

## 3. Diagnose the local range

Terminal B:

```bash
./build/stgs_port_check --host 127.0.0.1 --ports 9000:9010
```

The tool continues through the whole bounded range and reports OPEN/CLOSED/TIMEOUT/ERROR plus latency.

## 4. Send a message

Still in Terminal B:

```bash
./build/stgs_satellite_simulator \
  --tcp --host 127.0.0.1 --discover-ports 9000:9010 \
  --message "Telemetry link established" --count 1
```

Terminal A should print the sanitized message after the outer frame passes CRC validation.

## 5. Send a noisy sampled wave

```bash
./build/stgs_satellite_simulator \
  --tcp --host 127.0.0.1 --discover-ports 9000:9010 \
  --signal --sample-rate 200 --signal-frequency 5 \
  --signal-amplitude 1.0 --signal-samples 256 \
  --noise-stddev 0.35 --count 5 --rate 2 --seed 1234
```

Terminal A displays raw/filtered sparklines and signal metrics. Explain the separation:

- Gaussian noise is inside a valid frame and exercises filtering.
- `--corrupt` happens after CRC and exercises rejection.

## 6. Demonstrate deterministic concurrency

```bash
./build/stgs_satellite_simulator --output-file demo.stgf --count 1000 --rate 0 --seed 424242
./build/stgs_ground_station --replay demo.stgf --decoder-threads 1 --output one.csv
./build/stgs_ground_station --replay demo.stgf --decoder-threads 8 --output eight.csv
cmp one.csv eight.csv
```

The command should return success. Decoder workers finish out of order internally; the reorder stage preserves reception order externally.

## 7. Useful discussion points

- Why TCP requires framing while UDP does not.
- Why a `pollfd` snapshot must match exactly the client set being indexed.
- Why queues are bounded.
- Why CRC-32/ISO-HDLC is fully specified instead of merely saying “CRC32”.
- Why `0xEDB88320` is the reflected form of `0x04C11DB7`.
- Why a local OPEN port does not prove application identity without a handshake.
- Why health thresholds use hysteresis.
- Why multiple decoder threads are benchmarked rather than assumed faster.
- Why the signal filter is described as a demonstration DSP algorithm, not RF processing.

## 8. Scope statement

A strong presentation should also state what the project deliberately does **not** claim: certification, CCSDS compliance, TLS/authentication, real spacecraft hardware or RF demodulation. Precise scope makes the engineering claims more credible.
