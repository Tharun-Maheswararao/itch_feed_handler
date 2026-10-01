## Linux benchmark: c7i.2xlarge

Workflow run: https://github.com/Tharun-Maheswararao/itch_feed_handler/actions/runs/36871008026

| Host | |
|---|---|
| CPU | Intel(R) Xeon(R) Platinum 8488C (4 physical / 8 logical cores) |
| OS | Linux 7.0.0-1013-aws x86_64 |
| Compiler | gcc 13.3.0 |
| Flags | `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -march=native` |
| Clock | rdtsc, 0.42 ns/tick, smallest step between two reads 15.0 ns |
| Threads pinned | yes |

**End-to-end latency** (parser stamp → book updated), median of the runs:

| Configuration | Book msgs | Throughput (median, range) | p50 | p90 | p99 | p99.9 | max | book p50 | book p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| unpaced | 263,250,843 | 3.18 M/s (3.07–3.23) | 5.24 ms | 5.46 ms | 6.01 ms | 7.21 ms | 7.55 ms | 266 ns | 626 ns |
| paced_2M | 263,250,843 | 2.00 M/s (2.00–2.00) | 506 ns | 693 ns | 4.48 µs | 9.39 µs | 1.29 ms | 233 ns | 586 ns |
| paced_2M_view | 263,250,843 | 2.00 M/s (2.00–2.00) | 506 ns | 693 ns | 4.27 µs | 9.17 µs | 1.3 ms | 220 ns | 546 ns |

Run-to-run spread of end-to-end p99: unpaced 6.01 ms–6.12 ms; paced_2M 4.37 µs–4.59 µs; paced_2M_view 4.16 µs–4.48 µs

**Golden-model check (full day):** PASS: pipeline book matches the golden model

<details><summary>Component micro-benchmarks</summary>

```
268744780 messages, 263250843 book messages, best of 3
parse only                      2.795 s     10.6 ns/book msg     96.2 M msgs/s
parse + golden book            74.364 s    282.5 ns/book msg  (book only ~271.9 ns)
parse + fast book              24.504 s     93.1 ns/book msg  (book only ~82.5 ns)
fast book speedup over golden (book part): 3.30x
spsc transfer (2 threads)      15.770 s    78.85 ns/msg     12.7 M msgs/s
(checksum 1d3c200093233a6)
```

</details>

Charts: `docs/linux/*.png`. Raw data: `results/linux/*/run_stats.csv` and `histogram.csv`.
