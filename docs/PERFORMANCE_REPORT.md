# Performance Report: Space Telemetry Ground Station

## 1. Scope

This report documents the reproducible benchmark added for the decode pipeline. The goal is not to claim hardware-independent performance. The goal is to provide a repeatable way to evaluate parser, CRC and multithreaded queue throughput under controlled conditions.

The benchmark avoids network and disk I/O. It pre-generates valid binary telemetry frames, pushes them through the same `BlockingQueue<ByteVector>` and `decodeFrame()` path used by the ground station, and measures accepted frames per second.

## 2. Benchmark command

Build in Release mode:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run the benchmark:

```bash
./build/stgs_benchmark_decode --frames 200000 --payload-size 64 --decoder-threads 4
```

Available parameters:

```text
--frames <n>            Number of frames.
--payload-size <bytes>  Payload bytes per frame, maximum 4096.
--decoder-threads <n>   Number of decoder worker threads.
--seed <n>              Deterministic payload seed.
```

## 3. Reference measurements

Reference measurements were produced in a Linux container using a Release build. They should be treated as baseline evidence that the benchmark runs correctly, not as universal hardware claims.

| Frames | Payload size | Decoder threads | Decoded | Rejected | Duration seconds | Frames/second |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 50,000 | 64 bytes | 1 | 50,000 | 0 | 0.0348764 | 1,433,640 |
| 50,000 | 64 bytes | 2 | 50,000 | 0 | 0.0605646 | 825,565 |
| 50,000 | 256 bytes | 2 | 50,000 | 0 | 0.109293 | 457,488 |
| 50,000 | 1024 bytes | 2 | 50,000 | 0 | 0.192964 | 259,116 |

Example raw output:

```text
benchmark=decode_pipeline frames=50000 payload_size=64 decoder_threads=1 decoded=50000 rejected=0 duration_seconds=0.0348764 frames_per_second=1.43364e+06
benchmark=decode_pipeline frames=50000 payload_size=64 decoder_threads=2 decoded=50000 rejected=0 duration_seconds=0.0605646 frames_per_second=825565
benchmark=decode_pipeline frames=50000 payload_size=256 decoder_threads=2 decoded=50000 rejected=0 duration_seconds=0.109293 frames_per_second=457488
benchmark=decode_pipeline frames=50000 payload_size=1024 decoder_threads=2 decoded=50000 rejected=0 duration_seconds=0.192964 frames_per_second=259116
```

## 4. Interpretation

The benchmark is intentionally simple. It shows that the parser and CRC implementation can process high frame counts on normal Linux infrastructure. Throughput decreases as payload size increases because CRC calculation touches every byte.

The 1-thread result can be higher than the 2-thread result for small payloads because the queue synchronization overhead can dominate the work when frames are tiny. This is a useful result: it shows why benchmarking matters before increasing concurrency.

## 5. Reproducibility checklist

When comparing results, record:

- CPU model and core count;
- compiler and version;
- CMake build type;
- frame count;
- payload size;
- decoder thread count;
- whether logging, network and disk I/O are included.

For CI smoke testing, the project uses a smaller command:

```bash
./build/stgs_benchmark_decode --frames 10000 --payload-size 64 --decoder-threads 2
```
