## Queue benchmark matrix: Intel(R) Xeon(R) Platinum 8488C

30 cells, median of 3 runs each after one warmup. Linux 7.0.0-1013-aws x86_64, gcc 13.3.0, `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -march=native`. Latency sampled 1 in 16. unpaced = full-speed replay of 04:00–10:00; paced_itch = ITCH timestamps at 10x over 09:30–09:33.

Queues: `spsc` = this repo; `spsc_unaligned` = this repo; `rigtorp` = rigtorp/SPSCQueue@1053918; `boost` = boost 1_83; `mutex` = std::mutex + std::condition_variable.

| Load | Placement | Capacity | Queue | Throughput | p50 | p90 | p99 | p99.9 | max |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|
| unpaced | same-core | 64K | spsc | 9.05 M/s | 7.43 ms | 8.08 ms | 9.61 ms | 14 ms | 14.1 ms |
| unpaced | same-core | 64K | spsc_unaligned | 8.33 M/s | 8.08 ms | 8.52 ms | 10.3 ms | 14.8 ms | 14.8 ms |
| unpaced | same-core | 64K | rigtorp | 9.30 M/s | 7.43 ms | 7.86 ms | 9.61 ms | 14 ms | 14.1 ms |
| unpaced | same-core | 64K | boost | 9.08 M/s | 7.43 ms | 7.86 ms | 9.61 ms | 14 ms | 14 ms |
| unpaced | same-core | 64K | mutex | 3.68 M/s | 18.4 ms | 19.2 ms | 21.4 ms | 28 ms | 28 ms |
| unpaced | cross-core | 64K | spsc | 9.37 M/s | 7.43 ms | 7.65 ms | 8.96 ms | 13.3 ms | 13.3 ms |
| unpaced | cross-core | 64K | spsc_unaligned | 6.56 M/s | 10.3 ms | 10.9 ms | 12.5 ms | 18.4 ms | 18.4 ms |
| unpaced | cross-core | 64K | rigtorp | 7.89 M/s | 8.74 ms | 9.18 ms | 10.7 ms | 15.1 ms | 15.1 ms |
| unpaced | cross-core | 64K | boost | 6.94 M/s | 9.83 ms | 10.3 ms | 11.6 ms | 16.4 ms | 16.4 ms |
| unpaced | cross-core | 64K | mutex | 2.11 M/s | 32.3 ms | 33.2 ms | 35 ms | 40.7 ms | 40.7 ms |
| unpaced | cross-socket | 64K | spsc | 7.65 M/s | 8.52 ms | 9.18 ms | 13.1 ms | 69.9 ms | 70.1 ms |
| unpaced | cross-socket | 64K | spsc_unaligned | 3.75 M/s | 18.4 ms | 19.2 ms | 20.1 ms | 71.5 ms | 71.5 ms |
| unpaced | cross-socket | 64K | rigtorp | 4.93 M/s | 13.5 ms | 14.4 ms | 17 ms | 75.1 ms | 75.3 ms |
| unpaced | cross-socket | 64K | boost | 3.94 M/s | 17.5 ms | 17.9 ms | 21 ms | 71.7 ms | 71.8 ms |
| unpaced | cross-socket | 64K | mutex | 2.07 M/s | 32.3 ms | 34.1 ms | 38.4 ms | 82.1 ms | 85.7 ms |
| paced_itch | same-core | 64K | spsc | 0.52 M/s | 246 ns | 3.36 µs | 12.2 µs | 280 µs | 746 µs |
| paced_itch | same-core | 64K | spsc_unaligned | 0.52 M/s | 246 ns | 3.52 µs | 13.2 µs | 348 µs | 788 µs |
| paced_itch | same-core | 64K | rigtorp | 0.52 M/s | 240 ns | 3.25 µs | 11.7 µs | 287 µs | 945 µs |
| paced_itch | same-core | 64K | boost | 0.52 M/s | 240 ns | 3.36 µs | 12.4 µs | 321 µs | 781 µs |
| paced_itch | same-core | 64K | mutex | 0.50 M/s | 1.97 µs | 2.13 ms | 7.65 ms | 7.86 ms | 7.89 ms |
| paced_itch | cross-core | 64K | spsc | 0.52 M/s | 506 ns | 2.67 µs | 7.68 µs | 46.9 µs | 437 µs |
| paced_itch | cross-core | 64K | spsc_unaligned | 0.51 M/s | 733 ns | 14.1 µs | 1.91 ms | 2.29 ms | 2.31 ms |
| paced_itch | cross-core | 64K | rigtorp | 0.52 M/s | 626 ns | 2.83 µs | 7.89 µs | 59.7 µs | 528 µs |
| paced_itch | cross-core | 64K | boost | 0.51 M/s | 573 ns | 4.05 µs | 12.8 µs | 382 µs | 434 µs |
| paced_itch | cross-core | 64K | mutex | 0.48 M/s | 6.29 µs | 133 ms | 182 ms | 182 ms | 182 ms |
| paced_itch | cross-socket | 64K | spsc | 0.52 M/s | 1.25 µs | 6.4 µs | 328 µs | 50.7 ms | 55.2 ms |
| paced_itch | cross-socket | 64K | spsc_unaligned | 0.48 M/s | 2.24 µs | 68.2 ms | 94.4 ms | 99.6 ms | 99.9 ms |
| paced_itch | cross-socket | 64K | rigtorp | 0.51 M/s | 1.76 µs | 7.25 µs | 478 µs | 50.7 ms | 55.9 ms |
| paced_itch | cross-socket | 64K | boost | 0.50 M/s | 1.6 µs | 68.2 ms | 85.6 ms | 86.9 ms | 86.9 ms |
| paced_itch | cross-socket | 64K | mutex | 0.47 M/s | 9.81 µs | 206 ms | 292 ms | 292 ms | 292 ms |
