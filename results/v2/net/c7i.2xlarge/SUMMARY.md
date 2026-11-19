# Version 2 results: c7i.2xlarge

Paced at 2 M msgs/s; latency is the median of the clean runs' percentiles; every run is checked against the golden model.

| Path | Timestamp source | p50 | p99 | p99.9 | Max | Messages/s | Book match after loss |
|---|---|---:|---:|---:|---:|---:|---|
| File replay (Version 1, parse to book) | cycle counter (rdtsc) | 0.48 µs | 7.68 µs | 1.53 ms | 4.08 ms | 2.04 M | reference |
| Kernel sockets (one host, veth, multicast) | kernel software receive | 6.78 µs | 17.41 µs | 40.96 µs | 1.07 ms | 2.00 M | PASS: 4098 gaps (2651 other line, 1447 re-request, 0 snapshot) |
| AF_XDP (one host, veth, multicast) | user-space poll | 5.12 µs | 15.10 µs | 41.98 µs | 2.39 ms | 2.00 M | PASS: 1205 gaps (6 other line, 1199 re-request, 0 snapshot) |

## kernel UDP sockets (recvmmsg)

| Run | Passed | p50 | p99 | p99.9 | Max | Messages/s | Gaps | Kernel/NIC drops |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| clean 1 | yes | 6.78 µs | 17.41 µs | 40.96 µs | 1.07 ms | 2.00 M | 0 | 0 |
| clean 2 | yes | 6.91 µs | 17.41 µs | 79.87 µs | 1.21 ms | 2.00 M | 0 | 0 |
| clean 3 | yes | 6.78 µs | 16.38 µs | 24.57 µs | 389.03 µs | 2.00 M | 0 | 0 |
| seeded loss | yes | 10.75 µs | 155.19 ms | 171.97 ms | 188.10 ms | 2.00 M | 4098 | 0 |

Re-request recovery time: p50 1.25 ms, p99 119.54 ms; 2146 packets recovered. Checkpoints: 251 of 251 matched (level digest and queue order), final book matches. Heap allocations after warmup: 0.

Full speed: 0.18 M msgs/s received, 5 gaps (4 re-requested, 1 snapshot), 339664 kernel/NIC drops, book verified.

## AF_XDP (copy mode, generic XDP on vrx queue 0)

| Run | Passed | p50 | p99 | p99.9 | Max | Messages/s | Gaps | Kernel/NIC drops |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| clean 1 | yes | 5.12 µs | 16.13 µs | 116.73 µs | 2.39 ms | 2.00 M | 0 | 0 |
| clean 2 | yes | 5.12 µs | 15.10 µs | 41.98 µs | 1.19 ms | 2.00 M | 0 | 0 |
| clean 3 | yes | 5.12 µs | 15.10 µs | 40.96 µs | 7.84 ms | 2.00 M | 0 | 0 |
| seeded loss | yes | 7.68 µs | 155.19 ms | 167.77 ms | 180.66 ms | 2.00 M | 1205 | 1404495 |

Re-request recovery time: p50 1.28 ms, p99 65.01 ms; 1396721 packets recovered. Checkpoints: 251 of 251 matched (level digest and queue order), final book matches. Heap allocations after warmup: 0.

Full speed: 0.04 M msgs/s received, 5 gaps (4 re-requested, 0 snapshot), 95744 kernel/NIC drops, book NOT verified.
