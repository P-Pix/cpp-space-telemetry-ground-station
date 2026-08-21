# Technical Design: Space Telemetry Ground Station

## 1. Scope and intent

STGS is a C++20/Linux portfolio implementation of a small telemetry ground-station pipeline. Its purpose is to expose systems-programming decisions that can be reviewed directly: binary layout, byte order, CRC integrity, socket semantics, TCP framing, backpressure, ordering under concurrency, replay, diagnostics, DSP-like processing and failure handling.

It is not presented as certified flight/ground software and does not implement CCSDS, RF demodulation, authentication or a mission-specific operational standard.

## 2. Layered architecture

```text
                 +-----------------------------+
                 | stgs_satellite_simulator    |
                 | random / message / signal   |
                 +-------------+---------------+
                               |
                     UDP / TCP | STGF capture
                               v
+------------------+   +-------+--------+   +------------------+
| NetworkServer or |-->| bounded raw    |-->| decoder workers  |
| FrameFileReader  |   | work queue     |   | FrameCodec + CRC |
+------------------+   +----------------+   +--------+---------+
                                                    |
                                                    v
                                           +--------+---------+
                                           | bounded decoded  |
                                           | result queue     |
                                           +--------+---------+
                                                    |
                                                    v
                                           +--------+---------+
                                           | sequence reorder |
                                           +---+----+----+----+
                                               |    |    |
                           +-------------------+    |    +------------------+
                           v                        v                       v
                    StationHealth            STGA payload            CSV / JSON
                                            interpretation              export
                                               |
                                      +--------+---------+
                                      |                  |
                                   MESSAGE             SIGNAL
                                                       + filter
                                                       + metrics
```

The major boundary is deliberate: STGS validates transport/frame integrity first; STGA then interprets a valid payload. A malformed STGA envelope is therefore an application-payload error, not a CRC/frame failure.

## 3. STGS v1 wire format

All integer values are big-endian. `float` values are transported as their 32-bit IEEE-754 bit representation in big-endian order.

```text
MAGIC:u32
VERSION:u8
SATELLITE_ID:u16
TIMESTAMP_MS:u64
TEMPERATURE_C:f32
BATTERY_PERCENT:u8
STATUS:u8
PAYLOAD_LEN:u16
PAYLOAD:bytes[PAYLOAD_LEN]
CRC32:u32
```

The fixed header is 23 bytes. The implementation does not scatter offsets `5`, `7`, `15`, `19`, `20`, `21` and `23` through the parser: offsets are derived from named wire sizes in `TelemetryFrame.hpp` and a `static_assert` locks the v1 layout.

`MaxPayloadSize = 4096` is an application bound chosen to cap memory/replay/datagram sizes in this demonstration. It is not claimed to be a physical satellite limit.

## 4. CRC-32/ISO-HDLC

`Crc32.hpp` states the full CRC contract:

- width 32;
- polynomial `0x04C11DB7` in normal representation;
- reflected polynomial `0xEDB88320` for the LSB-first implementation;
- init `0xFFFFFFFF`;
- refin/refout true;
- xorout `0xFFFFFFFF`;
- standard check value `0xCBF43926` for `123456789`.

This is important for maintainability: `0xEDB88320` is not left as an unexplained literal inside the bit loop.

## 5. Frame decoding policy

`decodeFrame()` treats malformed input as an expected result and returns `std::variant<TelemetryFrame, FrameError>`. Validation order is intentionally defensive:

1. minimum length;
2. magic;
3. supported version;
4. bounded payload length;
5. exact total length;
6. CRC;
7. finite temperature and semantic field ranges such as battery/status.

No payload-derived allocation is performed before the encoded length has been bounded.

## 6. UDP behavior

UDP preserves datagram boundaries, so one datagram is one candidate frame. The receive buffer is `MaxFrameSize`, and `MSG_TRUNC` is used so an oversized datagram is detected and dropped instead of being silently interpreted from truncated bytes.

## 7. TCP behavior and the poll snapshot invariant

TCP does not preserve message boundaries. Every client owns an independent `StreamFrameExtractor`, which can handle:

- one frame split across many `recv()` calls;
- several frames in one `recv()`;
- noise before the magic;
- a magic split across reads;
- impossible payload lengths requiring resynchronization.

The multi-client loop follows a strict invariant:

1. snapshot the current client count;
2. build `pollfd[]` from exactly that snapshot;
3. call `poll()`;
4. process only those snapshot clients, in reverse order so erase cannot invalidate yet-to-process indices;
5. drain readable bytes even when `POLLHUP` is also signalled;
6. accept new clients only after snapshot processing.

A newly accepted client therefore cannot be indexed through a `pollfd` array that was built before the client existed. Client descriptors are RAII-owned so exception paths close them as well.

`NetworkServer::stop()` is cooperative: it only publishes an atomic stop request. The thread that owns the socket notices that request within the bounded `pollTimeoutMs` interval and closes its own descriptor. This avoids closing/reusing a file descriptor concurrently while another thread is blocked in `poll()`.

## 8. Bounded concurrent pipeline and deterministic output

Reception order receives a monotonic sequence number before the raw queue. Decoder workers may finish in any order. The writer stage stores decoded results keyed by sequence and only advances when the next expected result exists.

Consequences:

- CSV/JSON order is reception order;
- `StationHealthMonitor` observes exactly the same sequence with 1 or N workers;
- STGA messages/signals appear in deterministic order;
- worker scheduling cannot alter externally visible results.

Both handoff queues are bounded. When consumers lag, producers block rather than allowing unbounded heap growth.

A low-frequency terminal monitor samples the counters and queue sizes once per second when activity changes. It is intentionally separated from the data path: observability must not introduce per-frame terminal I/O or alter ordering semantics.

## 9. STGA application payload

STGA is a versioned mini-format inside the ordinary STGS payload. It starts with ASCII `STGA`, a version byte and a kind byte.

Current kinds:

- `TEXT_MESSAGE`: application sequence + text bytes;
- `SIGNAL_BLOCK`: sample rate, nominal frequency, nominal amplitude, absolute start sample index, sample count and float samples.

Payloads without the STGA magic remain opaque and valid. This keeps STGS v1 backward-compatible with pre-STGA random payloads and captures.

## 10. Signal generation, noise and filtering

The simulator generates a sinusoid with deterministic phase continuity across blocks. Optional Gaussian noise is added to the sample values **before** frame encoding and CRC calculation. This models a valid but noisy measurement.

`--corrupt`, in contrast, mutates encoded bytes **after** CRC generation. It models corruption and should be rejected by frame validation.

Two filters are available:

- centered moving average: small dependency-free FIR illustration;
- sine projection: projects samples onto DC, `sin(ωn)` and `cos(ωn)` at the known nominal frequency, then reconstructs the correlated component.

The reported SNR is an illustrative metric based on `raw - filtered`; it is not claimed to be an RF instrument measurement.

## 11. Port diagnostics

`stgs_port_check` and simulator discovery use non-blocking TCP `connect()` + `poll()` + `SO_ERROR`.

Safety/scope constraints are intentional:

- IPv4 loopback only (`127.0.0.0/8` / localhost);
- at most 256 ports in one command;
- bounded per-port timeout;
- sequential probes for predictable load and readable reporting.

An OPEN result proves that a TCP listener accepted the connection. STGS v1 has no handshake, so service identity is not inferred.

Ground-station `--auto-port` is a different operation: it tests which local candidate can be bound and selects the first available port.

## 12. Terminal observability

`TerminalUi` is dependency-free and adapts to the execution environment:

The ground-station process does not install a traditional asynchronous `std::signal` handler. SIGINT/SIGTERM are blocked before worker creation and consumed by a dedicated `std::jthread` using `sigtimedwait()`. The stop flag is therefore changed from ordinary thread context, where C++ synchronization primitives are safe to use, then the original signal mask is restored during shutdown.

For rendering:

- `isatty()` decides whether enhanced rendering is appropriate;
- `ioctl(TIOCGWINSZ)` adapts width;
- ANSI SGR is disabled for `NO_COLOR` or `TERM=dumb`;
- Unicode sparklines fall back to ASCII when appropriate;
- `std::osyncstream` keeps multi-threaded lines coherent;
- received text escapes control characters, including ESC, before display.

ANSI numeric values are named as ECMA-48/ISO 6429 SGR codes rather than being unexplained terminal constants.

## 13. Health monitoring and hysteresis

The health monitor uses a sliding window of accepted/rejected samples. Valid frames in `CRITICAL` or `SAFE_MODE` contribute to the critical count.

Default demonstration thresholds:

| Parameter | Value |
| --- | ---: |
| Window | 100 |
| Minimum samples | 20 |
| Reject-rate degrade | 0.10 |
| Reject-rate recover | 0.03 |
| Critical-count degrade | 8 |
| Critical-count recover | 3 |

Two hysteresis pairs prevent rapid state oscillation. The critical defaults are intentionally separated from the simulator's approximately 3 % default CRITICAL+SAFE_MODE background.

## 14. STGF replay

A replay starts with magic `STGF` and a named format version, followed by records:

```text
FRAME_LENGTH:u32_be | FRAME_BYTES
```

The writer validates frame length before write and checks stream errors. The reader validates the file header and bounds every frame length before allocation. `FRAME_BYTES` contains the complete original STGS frame, including CRC, so replay exercises the same decoder as live reception.

## 15. Error model

Expected untrusted-data failures return structured values:

- `FrameError` for STGS decode failures;
- `ApplicationPayloadError` for STGA failures;
- diagnostic states for port probes.

Exceptions represent failures that prevent the requested operation from continuing: invalid CLI/configuration, socket setup errors, impossible replay I/O and writer failures. Worker-thread exceptions are captured, pipeline queues are closed, and the exception is rethrown on the main thread.

## 16. Testing strategy

The dependency-free C++ test executable contains unit and real loopback integration cases covering:

- CRC reference vector;
- protocol round trip and malformed frames;
- TCP stream fragmentation/noise;
- STGA text/signal round trips;
- signal metadata validation (finite values and Nyquist bound);
- filter behavior;
- terminal-control sanitization;
- replay bounds;
- health hysteresis;
- queue backpressure;
- loopback-only port diagnostics;
- real TCP fragmentation followed by immediate peer close;
- cooperative server stop while the network thread is polling;
- real UDP receive.

The current suite contains **20 tests**. ASan/UBSan can be enabled with `STGS_ENABLE_SANITIZERS=ON` or `make asan-test`; ThreadSanitizer is available separately with `STGS_ENABLE_THREAD_SANITIZER=ON` or `make tsan-test`.

## 17. Performance philosophy

The benchmark isolates encode/decode/queue overhead from networking and disk. The project does not assume that “more threads = faster”: small frames can be synchronization-bound, so measurements are retained as evidence rather than making a scaling claim.

See `PERFORMANCE_REPORT.md`.

## 18. Deliberate limitations

A reviewer should be able to identify the project scope without reverse engineering it:

- no TLS, authentication or cryptographic authenticity;
- CRC detects accidental corruption, not malicious modification;
- no CCSDS compatibility claim;
- no real RF acquisition/demodulation;
- `poll()` is appropriate for this small demo; `epoll()` would be a natural direction for very large connection counts;
- manual JSON is intentionally small and controlled; a richer schema would justify a dedicated JSON library;
- health thresholds are configurable demonstration defaults, not operational mission limits.
