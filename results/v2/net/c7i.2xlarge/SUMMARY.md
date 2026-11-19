# Version 2 results: c7i.2xlarge

Paced at 2 M msgs/s; latency is the median of the clean runs' percentiles; every run is checked against the golden model.

| Path | Timestamp source | p50 | p99 | p99.9 | Max | Messages/s | Book match after loss |
|---|---|---:|---:|---:|---:|---:|---|
| File replay (Version 1, parse to book) | cycle counter (rdtsc) | 0.47 µs | 4.05 µs | 8.11 µs | 1.13 ms | 2.04 M | reference |
| Kernel sockets (one host, veth, multicast) | kernel software receive | 6.78 µs | 16.38 µs | 22.53 µs | 153.05 µs | 2.00 M | PASS: 3983 gaps (2551 other line, 1432 re-request, 0 snapshot) |
| AF_XDP (one host, veth, multicast) | user-space poll | 4.86 µs | 13.57 µs | 19.45 µs | 136.33 µs | 2.00 M | PASS: 1198 gaps (6 other line, 1192 re-request, 0 snapshot) |

## kernel UDP sockets (recvmmsg)

| Run | Passed | p50 | p99 | p99.9 | Max | Messages/s | Gaps | Kernel/NIC drops |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| clean 1 | yes | 6.78 µs | 16.38 µs | 22.53 µs | 153.05 µs | 2.00 M | 0 | 0 |
| clean 2 | yes | 6.78 µs | 16.38 µs | 22.53 µs | 3.66 ms | 2.00 M | 0 | 0 |
| clean 3 | yes | 6.91 µs | 16.89 µs | 22.53 µs | 149.88 µs | 2.00 M | 0 | 0 |
| seeded loss | yes | 11.26 µs | 146.80 ms | 155.19 ms | 165.17 ms | 2.00 M | 3983 | 0 |

Re-request recovery time: p50 1.21 ms, p99 109.05 ms; 2276 packets recovered. Checkpoints: 251 of 251 matched (level digest and queue order), final book matches. Heap allocations after warmup: 0.

Full speed: the publisher sent 6.41 M msgs/s; 668395 kernel/NIC drops; 6 gaps: 0 filled by the other line, 5 by re-request (174212 packets), 4 snapshot loads; book verified at every checkpoint after the last snapshot and at the end. This is a stress test of recovery, not a throughput figure: what the receiver could not take directly came from re-requests and snapshots.

## AF_XDP (copy mode, generic XDP on vrx queue 0)

| Run | Passed | p50 | p99 | p99.9 | Max | Messages/s | Gaps | Kernel/NIC drops |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| clean 1 | yes | 5.25 µs | 14.59 µs | 20.48 µs | 126.83 µs | 2.00 M | 0 | 0 |
| clean 2 | yes | 4.86 µs | 13.57 µs | 19.45 µs | 136.33 µs | 2.00 M | 0 | 0 |
| clean 3 | yes | 4.61 µs | 12.54 µs | 17.92 µs | 204.00 µs | 2.00 M | 0 | 0 |
| seeded loss | yes | 10.49 µs | 146.80 ms | 155.19 ms | 167.17 ms | 2.00 M | 1198 | 0 |

Re-request recovery time: p50 1.15 ms, p99 13.11 ms; 2616 packets recovered. Checkpoints: 251 of 251 matched (level digest and queue order), final book matches. Heap allocations after warmup: 0.

Full speed: the publisher sent 12.35 M msgs/s; 88541 kernel/NIC drops; 2 gaps: 0 filled by the other line, 1 by re-request (191100 packets), 2 snapshot loads; book verified at every checkpoint after the last snapshot and at the end. This is a stress test of recovery, not a throughput figure: what the receiver could not take directly came from re-requests and snapshots.
