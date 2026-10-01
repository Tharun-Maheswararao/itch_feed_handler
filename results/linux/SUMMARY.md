## Linux benchmark: c7i.2xlarge

Workflow run: https://github.com/Tharun-Maheswararao/itch_feed_handler/actions/runs/36891000656

| Host | |
|---|---|
| CPU | Intel(R) Xeon(R) Platinum 8488C (4 physical / 8 logical cores) |
| OS | Linux 7.0.0-1013-aws x86_64 |
| Compiler | gcc 13.3.0 |
| Flags | `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -march=native` |
| Clock | rdtsc, 0.42 ns/tick, smallest step between two reads 13.3 ns |
| Threads pinned | yes |

**End-to-end latency** (parser stamp → book updated), median of the runs:

| Configuration | Book msgs | Timed | Throughput (median, range) | p50 | p90 | p99 | p99.9 | max | book p50 | book p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| unpaced | 263,250,843 | 1/16 | 7.69 M/s (7.47–7.73) | 2.13 ms | 2.29 ms | 2.68 ms | 3.71 ms | 4.5 ms | 146 ns | 493 ns |
| paced_2M | 263,250,843 | 1/16 | 2.00 M/s (2.00–2.00) | 466 ns | 680 ns | 3.09 µs | 8.75 µs | 1.3 ms | 153 ns | 586 ns |
| paced_2M_every | 263,250,843 | all | 2.00 M/s (2.00–2.00) | 466 ns | 693 ns | 4.59 µs | 8.75 µs | 1.28 ms | 233 ns | 640 ns |
| paced_2M_view | 263,250,843 | 1/16 | 2.00 M/s (2.00–2.00) | 480 ns | 706 ns | 3.25 µs | 8.75 µs | 1.28 ms | 153 ns | 600 ns |

Run-to-run spread of end-to-end p99: unpaced 2.62 ms–2.73 ms; paced_2M 2.99 µs–3.25 µs; paced_2M_every 4.37 µs–5.33 µs; paced_2M_view 3.04 µs–3.41 µs

**Golden-model check (full day):** PASS: pipeline book matches the golden model

<details><summary>Component micro-benchmarks</summary>

```
268744780 messages, 263250843 book messages, best of 3
parse only                      2.774 s     10.5 ns/book msg     96.9 M msgs/s
parse + golden book            86.844 s    329.9 ns/book msg  (book only ~319.4 ns)
parse + fast book              25.380 s     96.4 ns/book msg  (book only ~85.9 ns)
fast book speedup over golden (book part): 3.72x
spsc transfer, batch 1          9.114 s    45.57 ns/msg     21.9 M msgs/s
spsc transfer, batch 8          5.144 s    25.72 ns/msg     38.9 M msgs/s
spsc transfer, batch 32         3.273 s    16.36 ns/msg     61.1 M msgs/s
(checksum 261ddc9bc4a71a6)
```

</details>

Charts: `docs/linux/*.png`. Raw data: `results/linux/*/run_stats.csv` and `histogram.csv`.
