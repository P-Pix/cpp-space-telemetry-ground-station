# STGS decode benchmark

## What I wanted to measure

The decoding pipeline has a few costs that are easy to underestimate: reading a binary frame, checking its CRC and moving work between threads. I added a small benchmark to compare these costs with different payload sizes and worker counts.

It deliberately leaves out network reception and file writing. Frames are created in memory and passed through a `BlockingQueue<ByteVector>` and `decodeFrame()`. The measured throughput therefore belongs to this benchmark, not to a complete ground station.

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

## 4. What I take from these runs

The CRC needs to read the bytes of the frame, so increasing the payload size naturally costs more. I also find the first two measurements interesting: with very small frames, one decoder can be quicker than two. The extra synchronization and contention can outweigh the parallel work.

These were individual runs in a Linux container, not a controlled performance study. Before using these numbers to size a real pipeline, I would repeat the tests on the target machine, include the actual socket and disk I/O, and look at latency as well as throughput.

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
