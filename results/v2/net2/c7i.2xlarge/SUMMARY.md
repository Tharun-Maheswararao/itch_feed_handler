# Version 2 results: two hosts (ip-172-31-90-229)

Paced at 2 M msgs/s; latency is the median of the clean runs' percentiles; every run is checked against the golden model.

| Path | Timestamp source | p50 | p99 | p99.9 | Max | Messages/s | Book match after loss |
|---|---|---:|---:|---:|---:|---:|---|
| Kernel sockets (two hosts, unicast) | kernel software receive | 15.36 µs | 37.89 µs | 51.20 µs | 573.31 µs | 2.00 M | PASS: 5724 gaps (4237 other line, 1487 re-request, 0 snapshot) |
| AF_XDP (two hosts, unicast) | user-space poll | 11.26 µs | 31.23 µs | 44.03 µs | 196.47 µs | 2.00 M | PASS: 1173 gaps (7 other line, 1166 re-request, 0 snapshot) |

## kernel UDP sockets (recvmmsg)

| Run | Passed | p50 | p99 | p99.9 | Max | Messages/s | Gaps | Kernel/NIC drops |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| clean 1 | yes | 15.36 µs | 38.91 µs | 52.22 µs | 573.31 µs | 2.00 M | 0 | 0 |
| clean 2 | yes | 14.85 µs | 37.89 µs | 51.20 µs | 2.81 ms | 2.00 M | 0 | 0 |
| clean 3 | yes | 15.36 µs | 37.89 µs | 50.17 µs | 229.23 µs | 2.00 M | 0 | 0 |
| seeded loss | yes | 3.67 ms | 163.58 ms | 176.16 ms | 191.04 ms | 2.00 M | 5724 | 0 |

Re-request recovery time: p50 1.70 ms, p99 127.93 ms; 2550 packets recovered. Checkpoints: 251 of 251 matched (level digest and queue order), final book matches. Heap allocations after warmup: 0.

Full speed: the publisher sent as fast as it could; 391836 kernel/NIC drops; 7 gaps: 0 filled by the other line, 6 by re-request (171730 packets), 5 snapshot loads; book verified at every checkpoint after the last snapshot and at the end. This is a stress test of recovery, not a throughput figure: what the receiver could not take directly came from re-requests and snapshots.

## AF_XDP (copy mode, generic XDP on enp39s0 queue 0)

| Run | Passed | p50 | p99 | p99.9 | Max | Messages/s | Gaps | Kernel/NIC drops |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| clean 1 | yes | 11.52 µs | 32.26 µs | 45.05 µs | 216.55 µs | 2.00 M | 0 | 0 |
| clean 2 | yes | 11.01 µs | 31.23 µs | 44.03 µs | 196.47 µs | 2.00 M | 0 | 0 |
| clean 3 | yes | 11.26 µs | 31.23 µs | 43.01 µs | 167.70 µs | 2.00 M | 0 | 0 |
| seeded loss | yes | 5.11 ms | 163.58 ms | 176.16 ms | 191.03 ms | 2.00 M | 1173 | 0 |

Re-request recovery time: p50 1.44 ms, p99 17.30 ms; 2775 packets recovered. Checkpoints: 251 of 251 matched (level digest and queue order), final book matches. Heap allocations after warmup: 0.

Full speed: the publisher sent as fast as it could; 323380 kernel/NIC drops; 1 gaps: 0 filled by the other line, 0 by re-request (303865 packets), 4 snapshot loads; book verified at every checkpoint after the last snapshot and at the end. This is a stress test of recovery, not a throughput figure: what the receiver could not take directly came from re-requests and snapshots.
