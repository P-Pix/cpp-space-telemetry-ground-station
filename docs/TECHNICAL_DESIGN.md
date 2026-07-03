# Technical Design: Space Telemetry Ground Station

## 1. Purpose

This project implements a compact Linux-based telemetry ground station in modern C++20. It receives simulated spacecraft telemetry over UDP or TCP, reconstructs binary frames, validates CRC-32 integrity, decodes telemetry fields, logs frame errors, exports accepted frames to CSV or JSON and can replay previously captured STGF files.

The project is designed as an interview-quality systems project: it exercises sockets, binary protocols, multithreading, CMake, testing, CI/CD, error handling, degraded operational behavior and telemetry-domain concepts without requiring real space hardware.

## 2. Architecture

```text
                  +-------------------------+
                  | stgs_satellite_simulator|
                  |  UDP/TCP or STGF file   |
                  +------------+------------+
                               |
                               v
+------------------+    +------+-------+    +----------------+    +---------------+
| Network receiver | -> | Raw queue    | -> | Decoder workers| -> | Decoded queue |
| or replay reader |    | ByteVector   |    | CRC + parsing  |    | TelemetryFrame|
+------------------+    +--------------+    +------+---------+    +-------+-------+
                                                   |                      |
                                                   v                      v
                                          +--------+---------+      +------+------+
                                          | Health monitor   |      | CSV/JSON    |
                                          | nominal/degraded |      | writer      |
                                          +------------------+      +-------------+
```

### Components

- `TelemetryFrame`: domain model for a decoded frame.
- `FrameCodec`: binary encoder/decoder and TCP stream frame extractor.
- `Crc32`: standard CRC-32 implementation.
- `BlockingQueue`: closeable producer/consumer queue for thread handoff.
- `Logger`: thread-safe console/file logger.
- `NetworkServer`: Linux UDP/TCP receiver using POSIX sockets and `poll()`.
- `Replay`: `.stgf` binary capture reader/writer.
- `StationHealth`: operational monitor for nominal/degraded station state.
- `ground_station`: CLI application that connects networking/replay with decoder workers, degraded mode and exporters.
- `satellite_simulator`: configurable telemetry generator.
- `stgs_benchmark_decode`: reproducible benchmark for the decode pipeline.

## 3. Binary protocol

All integer fields are encoded in network byte order, also called big-endian. The frame starts with a fixed magic word so that TCP streams can resynchronize if bytes are lost, corrupted or concatenated.

```text
MAGIC | VERSION | SATELLITE_ID | TIMESTAMP_MS | TEMPERATURE_C |
BATTERY_PERCENT | STATUS | PAYLOAD_LEN | PAYLOAD | CRC32
```

The fixed header is 23 bytes and the CRC is 4 bytes. `PAYLOAD_LEN` is included because a binary protocol needs a safe way to delimit variable-sized payloads. Maximum payload size is 4096 bytes.

The CRC-32 field covers every byte before the CRC field. Frames with a bad CRC are rejected before their telemetry values are trusted.

Strict validation rules:

- the frame must start with the `STGS` magic word;
- the protocol version must match the supported version;
- the payload length must not exceed `MaxPayloadSize`;
- the actual byte count must match the encoded payload length;
- the CRC must match the frame contents;
- battery percentage must be between `0` and `100`;
- status must be one of the known enum values.

## 4. UDP and TCP behavior

### UDP

UDP mode expects exactly one complete telemetry frame per datagram. Loss is expected and can be simulated by the telemetry generator. The receiver validates each datagram independently.

### TCP

TCP mode is a byte stream, so frame boundaries are not preserved by the transport. `StreamFrameExtractor` buffers incoming bytes, searches for the `STGS` magic word, reads `PAYLOAD_LEN`, waits for the complete frame and emits complete binary frames to the decoder queue.

The extractor discards noise before the magic word and keeps up to three trailing bytes when no magic word is currently visible, allowing magic bytes to be split across network reads.

## 5. Multithreading model

The ground station uses a pipeline:

1. The network receiver or replay reader pushes raw frame bytes into `BlockingQueue<ByteVector>`.
2. A configurable number of decoder worker threads pop raw frames, call `decodeFrame()` and push validated telemetry frames into `BlockingQueue<TelemetryFrame>`.
3. The station health monitor records accepted and rejected samples.
4. The writer thread writes decoded frames to CSV or JSON.

This design separates I/O latency from CPU-bound parsing and gives a simple route to thousands of frames per second on normal Linux machines.

## 6. Degraded mode

`StationHealthMonitor` implements the operational degraded mode. It maintains a sliding window of recent samples. Each sample is either:

- a decoded telemetry frame, optionally marked as critical when the frame status is `CRITICAL` or `SAFE_MODE`;
- a rejected frame, usually caused by CRC mismatch, bad magic, length mismatch or unsupported version.

The station changes state from `NOMINAL` to `DEGRADED` when one of these conditions is true after the minimum sample count is reached:

- the rejection rate in the sliding window is greater than or equal to the configured degraded threshold;
- the number of critical telemetry frames in the window reaches the configured critical threshold.

The station returns from `DEGRADED` to `NOMINAL` when the rejection rate falls below the configured recovery threshold and the window no longer contains critical telemetry.

Default values:

| Setting | Default |
| --- | ---: |
| Window size | `100` samples |
| Minimum samples | `20` samples |
| Degraded rejection threshold | `0.10` |
| Recovery rejection threshold | `0.03` |
| Critical telemetry threshold | `3` |

The mode can be disabled with `--disable-degraded` for pure throughput experiments.

## 7. Error handling

`decodeFrame()` returns `std::variant<TelemetryFrame, FrameError>` rather than throwing for normal parse failures. This makes malformed telemetry an expected data condition, not a control-flow exception. Structural errors include:

- frame too short;
- invalid magic word;
- unsupported protocol version;
- payload larger than the configured maximum;
- length mismatch;
- CRC mismatch;
- battery outside the `0..100` range;
- unknown status value.

I/O setup errors, invalid CLI arguments and replay file corruption use exceptions and terminate the CLI with a clear error message.

## 8. Replay format

Replay files use the `.stgf` extension by convention. The file starts with eight bytes:

```text
'S' 'T' 'G' 'F' 0 0 0 1
```

Each stored frame is then encoded as:

```text
FRAME_LENGTH:uint32_be | FRAME_BYTES
```

`FRAME_BYTES` contains the original telemetry frame, including its CRC. This allows replay to exercise the exact same parser and validation path as live telemetry.

## 9. Export formats

The ground station supports two export formats:

- CSV for simple spreadsheet-like inspection;
- JSON for integration with tools, dashboards or automated checks.

The format is inferred from the output extension when possible. It can also be forced with `--output-format csv` or `--output-format json`.

Exported fields:

- `timestamp_ms`;
- `satellite_id`;
- `temperature_c`;
- `battery_percent`;
- `status`;
- `payload_len`;
- `payload_hex`.

## 10. Testing strategy

The unit test executable covers:

- CRC-32 known vector `123456789 -> 0xCBF43926`;
- encode/decode round trip;
- bad magic rejection;
- CRC mismatch rejection;
- length mismatch rejection;
- TCP stream extraction with noise and split frames;
- replay writer/reader round trip;
- station health degraded/recovery transition;
- blocking queue close behavior.

The tests are deliberately dependency-free so the project can build on a minimal Linux environment.

The GitHub Actions workflow adds smoke checks for:

- building in Release mode;
- running CTest;
- generating an STGF replay file;
- replaying to CSV;
- replaying to JSON and validating it with Python;
- running a small benchmark smoke test.

## 11. Performance considerations

- The decoder validates frame structure and CRC before trusting telemetry values.
- The receiver and writer are decoupled by queues.
- Decoder thread count is configurable with `--decoder-threads`.
- The TCP extractor keeps a bounded payload size and rejects impossible frame lengths early.
- Logging is thread-safe but should be kept at `info` or higher for high-rate runs to avoid I/O bottlenecks.
- `stgs_benchmark_decode` isolates decode pipeline throughput from network and disk overhead.

See `docs/PERFORMANCE_REPORT.md` for reproducible benchmark commands and reference results.

## 12. Possible extensions

- Prometheus metrics exporter.
- WebSocket dashboard.
- Multiple satellites with per-satellite statistics.
- Binary capture rotation by size or time.
- CCSDS-inspired primary header.
- Sequence counters to estimate UDP packet loss from telemetry itself.
- Property-based fuzz tests for the parser.
- Linux systemd service unit for persistent ground-station deployment.
