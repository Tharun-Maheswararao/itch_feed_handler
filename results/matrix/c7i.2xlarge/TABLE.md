## Queue benchmark matrix: Intel(R) Xeon(R) Platinum 8488C

60 cells, median of 3 runs each after one warmup. Linux 7.0.0-1013-aws x86_64, gcc 13.3.0, `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -march=native`. Latency sampled 1 in 16. unpaced = full-speed replay of 04:00–10:00; paced_itch = ITCH timestamps at 10x over 09:30–09:33.

Queues: `spsc` = this repo; `spsc_unaligned` = this repo; `rigtorp` = rigtorp/SPSCQueue@1053918; `boost` = boost 1_83; `mutex` = std::mutex + std::condition_variable.

| Load | Placement | Capacity | Queue | Throughput | p50 | p90 | p99 | p99.9 | max |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|
| unpaced | same-core | 1K | spsc | 6.71 M/s | 157 µs | 177 µs | 215 µs | 314 µs | 1.5 ms |
| unpaced | same-core | 1K | spsc_unaligned | 6.19 M/s | 171 µs | 191 µs | 232 µs | 321 µs | 1.41 ms |
| unpaced | same-core | 1K | rigtorp | 6.97 M/s | 150 µs | 174 µs | 208 µs | 307 µs | 1.4 ms |
| unpaced | same-core | 1K | boost | 6.75 M/s | 157 µs | 177 µs | 212 µs | 307 µs | 1.34 ms |
| unpaced | same-core | 1K | mutex | 2.65 M/s | 396 µs | 430 µs | 478 µs | 614 µs | 1.4 ms |
| unpaced | same-core | 64K | spsc | 6.60 M/s | 10.3 ms | 11.1 ms | 13.3 ms | 18.8 ms | 19.2 ms |
| unpaced | same-core | 64K | spsc_unaligned | 6.09 M/s | 11.1 ms | 12 ms | 13.8 ms | 20.1 ms | 20.1 ms |
| unpaced | same-core | 64K | rigtorp | 6.73 M/s | 10 ms | 11.1 ms | 13.3 ms | 19.6 ms | 19.6 ms |
| unpaced | same-core | 64K | boost | 6.56 M/s | 10.3 ms | 11.1 ms | 14.4 ms | 19.2 ms | 19.4 ms |
| unpaced | same-core | 64K | mutex | 2.60 M/s | 25.8 ms | 27.1 ms | 30.6 ms | 37.4 ms | 37.4 ms |
| unpaced | same-core | 1M | spsc | 6.46 M/s | 171 ms | 182 ms | 196 ms | 198 ms | 198 ms |
| unpaced | same-core | 1M | spsc_unaligned | 6.03 M/s | 182 ms | 189 ms | 209 ms | 209 ms | 209 ms |
| unpaced | same-core | 1M | rigtorp | 6.62 M/s | 164 ms | 178 ms | 192 ms | 194 ms | 194 ms |
| unpaced | same-core | 1M | boost | 6.49 M/s | 168 ms | 182 ms | 196 ms | 196 ms | 196 ms |
| unpaced | same-core | 1M | mutex | 2.64 M/s | 419 ms | 426 ms | 445 ms | 445 ms | 445 ms |
| unpaced | cross-core | 1K | spsc | 7.16 M/s | 147 µs | 167 µs | 198 µs | 287 µs | 1.06 ms |
| unpaced | cross-core | 1K | spsc_unaligned | 4.18 M/s | 253 µs | 273 µs | 314 µs | 403 µs | 1.19 ms |
| unpaced | cross-core | 1K | rigtorp | 5.83 M/s | 181 µs | 205 µs | 239 µs | 335 µs | 1.11 ms |
| unpaced | cross-core | 1K | boost | 5.72 M/s | 184 µs | 205 µs | 239 µs | 335 µs | 1.13 ms |
| unpaced | cross-core | 1K | mutex | 2.20 M/s | 478 µs | 505 µs | 546 µs | 628 µs | 1.3 ms |
| unpaced | cross-core | 64K | spsc | 7.10 M/s | 9.61 ms | 10.3 ms | 11.4 ms | 20.1 ms | 21.1 ms |
| unpaced | cross-core | 64K | spsc_unaligned | 4.90 M/s | 13.8 ms | 14.9 ms | 16.2 ms | 22.7 ms | 22.9 ms |
| unpaced | cross-core | 64K | rigtorp | 6.08 M/s | 11.1 ms | 12 ms | 13.5 ms | 19.6 ms | 19.6 ms |
| unpaced | cross-core | 64K | boost | 5.60 M/s | 12.2 ms | 12.9 ms | 14.4 ms | 21 ms | 21.1 ms |
| unpaced | cross-core | 64K | mutex | 2.23 M/s | 30.6 ms | 31.5 ms | 33.2 ms | 38.4 ms | 38.5 ms |
| unpaced | cross-core | 1M | spsc | 7.08 M/s | 157 ms | 164 ms | 171 ms | 172 ms | 172 ms |
| unpaced | cross-core | 1M | spsc_unaligned | 4.82 M/s | 231 ms | 245 ms | 254 ms | 254 ms | 254 ms |
| unpaced | cross-core | 1M | rigtorp | 5.78 M/s | 192 ms | 199 ms | 216 ms | 216 ms | 216 ms |
| unpaced | cross-core | 1M | boost | 5.66 M/s | 196 ms | 203 ms | 218 ms | 218 ms | 218 ms |
| unpaced | cross-core | 1M | mutex | 1.90 M/s | 573 ms | 587 ms | 596 ms | 596 ms | 596 ms |
| paced_itch | same-core | 1K | spsc | 0.52 M/s | 466 ns | 20.5 µs | 1.97 ms | 2.35 ms | 2.35 ms |
| paced_itch | same-core | 1K | spsc_unaligned | 0.52 M/s | 453 ns | 14.5 µs | 1.86 ms | 2.24 ms | 2.26 ms |
| paced_itch | same-core | 1K | rigtorp | 0.52 M/s | 453 ns | 13.4 µs | 1.09 ms | 1.45 ms | 1.47 ms |
| paced_itch | same-core | 1K | boost | 0.52 M/s | 453 ns | 14.1 µs | 1.23 ms | 1.64 ms | 1.65 ms |
| paced_itch | same-core | 1K | mutex | 0.49 M/s | 2.93 µs | 69.9 ms | 96.1 ms | 101 ms | 101 ms |
| paced_itch | same-core | 64K | spsc | 0.52 M/s | 466 ns | 19.2 µs | 2.24 ms | 2.57 ms | 2.58 ms |
| paced_itch | same-core | 64K | spsc_unaligned | 0.52 M/s | 466 ns | 14.9 µs | 1.67 ms | 2.11 ms | 2.11 ms |
| paced_itch | same-core | 64K | rigtorp | 0.52 M/s | 466 ns | 14.1 µs | 1.72 ms | 2.13 ms | 2.16 ms |
| paced_itch | same-core | 64K | boost | 0.52 M/s | 466 ns | 14.5 µs | 1.8 ms | 2.18 ms | 2.19 ms |
| paced_itch | same-core | 64K | mutex | 0.49 M/s | 2.88 µs | 45.4 ms | 73.4 ms | 77.9 ms | 77.9 ms |
| paced_itch | same-core | 1M | spsc | 0.52 M/s | 466 ns | 13.4 µs | 1.39 ms | 1.8 ms | 1.82 ms |
| paced_itch | same-core | 1M | spsc_unaligned | 0.52 M/s | 466 ns | 17.1 µs | 1.97 ms | 2.34 ms | 2.34 ms |
| paced_itch | same-core | 1M | rigtorp | 0.52 M/s | 466 ns | 13.7 µs | 1.39 ms | 1.8 ms | 1.83 ms |
| paced_itch | same-core | 1M | boost | 0.52 M/s | 466 ns | 18.3 µs | 1.91 ms | 2.24 ms | 2.25 ms |
| paced_itch | same-core | 1M | mutex | 0.49 M/s | 2.72 µs | 8.74 ms | 27.1 ms | 28.8 ms | 29 ms |
| paced_itch | cross-core | 1K | spsc | 0.52 M/s | 666 ns | 4.27 µs | 31.6 µs | 601 µs | 620 µs |
| paced_itch | cross-core | 1K | spsc_unaligned | 0.51 M/s | 813 ns | 2.95 ms | 9.83 ms | 10.6 ms | 10.6 ms |
| paced_itch | cross-core | 1K | rigtorp | 0.51 M/s | 720 ns | 4.8 µs | 737 µs | 928 µs | 972 µs |
| paced_itch | cross-core | 1K | boost | 0.51 M/s | 800 ns | 12.6 µs | 3.28 ms | 3.44 ms | 3.48 ms |
| paced_itch | cross-core | 1K | mutex | 0.47 M/s | 8.32 µs | 185 ms | 265 ms | 265 ms | 265 ms |
| paced_itch | cross-core | 64K | spsc | 0.52 M/s | 706 ns | 4.59 µs | 21.3 µs | 396 µs | 455 µs |
| paced_itch | cross-core | 64K | spsc_unaligned | 0.51 M/s | 826 ns | 10.5 µs | 1.01 ms | 1.28 ms | 1.29 ms |
| paced_itch | cross-core | 64K | rigtorp | 0.51 M/s | 773 ns | 4.48 µs | 19.2 µs | 355 µs | 419 µs |
| paced_itch | cross-core | 64K | boost | 0.51 M/s | 800 ns | 10.2 µs | 983 µs | 1.28 ms | 1.3 ms |
| paced_itch | cross-core | 64K | mutex | 0.46 M/s | 8.11 µs | 192 ms | 273 ms | 276 ms | 276 ms |
| paced_itch | cross-core | 1M | spsc | 0.52 M/s | 693 ns | 4.27 µs | 17.5 µs | 328 µs | 380 µs |
| paced_itch | cross-core | 1M | spsc_unaligned | 0.51 M/s | 813 ns | 9.17 µs | 683 µs | 901 µs | 906 µs |
| paced_itch | cross-core | 1M | rigtorp | 0.52 M/s | 720 ns | 4.37 µs | 18.3 µs | 348 µs | 404 µs |
| paced_itch | cross-core | 1M | boost | 0.51 M/s | 773 ns | 10 µs | 751 µs | 956 µs | 959 µs |
| paced_itch | cross-core | 1M | mutex | 0.48 M/s | 7.04 µs | 55.1 ms | 78.6 ms | 79.8 ms | 79.8 ms |
