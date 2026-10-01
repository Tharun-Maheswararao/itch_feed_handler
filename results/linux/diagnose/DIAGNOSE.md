## Pipeline diagnosis (until 12:00:00)

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
| unpaced_b1 | 132,659,542 | 3.92 M/s (3.91–3.92) | 4.26 ms | 4.59 ms | 5.02 ms | 6.34 ms | 7.47 ms | 186 ns | 640 ns |
| unpaced_b1_nolat | 132,659,542 | 7.41 M/s (7.37–7.47) | n/a | n/a | n/a | n/a | n/a | n/a | n/a |
| unpaced_b1_smt | 132,659,542 | 4.15 M/s (4.13–4.17) | 4.04 ms | 4.26 ms | 4.7 ms | 5.57 ms | 7.35 ms | 173 ns | 640 ns |
| unpaced_b32 | 132,659,542 | 4.23 M/s (4.19–4.23) | 3.93 ms | 4.26 ms | 4.59 ms | 5.35 ms | 6.62 ms | 170 ns | 626 ns |
| unpaced_b32_nolat | 132,659,542 | 7.83 M/s (7.81–8.00) | n/a | n/a | n/a | n/a | n/a | n/a | n/a |
| unpaced_b32_smt | 132,659,542 | 4.14 M/s (4.14–4.15) | 4.04 ms | 4.37 ms | 4.7 ms | 5.57 ms | 7.29 ms | 170 ns | 640 ns |
| unpaced_b8 | 132,659,542 | 4.31 M/s (4.30–4.33) | 3.82 ms | 4.15 ms | 4.48 ms | 5.35 ms | 6.31 ms | 166 ns | 613 ns |
| paced_2M_b1 | 132,659,542 | 2.00 M/s (2.00–2.00) | 506 ns | 800 ns | 5.87 µs | 10.5 µs | 1.38 ms | 253 ns | 733 ns |
| paced_2M_b32 | 132,659,542 | 2.00 M/s (2.00–2.00) | 506 ns | 840 ns | 6.72 µs | 14.1 µs | 1.36 ms | 253 ns | 746 ns |

Run-to-run spread of end-to-end p99: unpaced_b1 5.02 ms–5.02 ms; unpaced_b1_smt 4.7 ms–4.7 ms; unpaced_b32 4.59 ms–4.59 ms; unpaced_b32_smt 4.7 ms–4.7 ms; unpaced_b8 4.48 ms–4.48 ms; paced_2M_b1 5.87 µs–6.08 µs; paced_2M_b32 6.29 µs–7.25 µs

n/a: run with `--no-latency` (no per-message clock reads), to measure throughput only.

<details><summary>Component micro-benchmarks</summary>

```
268744780 messages, 263250843 book messages, best of 1
parse only                      2.958 s     11.2 ns/book msg     90.9 M msgs/s
parse + golden book           107.394 s    408.0 ns/book msg  (book only ~396.7 ns)
parse + fast book              29.214 s    111.0 ns/book msg  (book only ~99.7 ns)
fast book speedup over golden (book part): 3.98x
spsc transfer, batch 1, pinned   18.508 s    92.54 ns/msg     10.8 M msgs/s
spsc transfer, batch 8, pinned    5.860 s    29.30 ns/msg     34.1 M msgs/s
spsc transfer, batch 32, pinned    3.421 s    17.11 ns/msg     58.5 M msgs/s
(checksum 15965b79c8663e2)
```

</details>

Charts: `(none)/*.png`. Raw data: `results/diagnose/*/run_stats.csv` and `histogram.csv`.

### Single-threaded book (hypothesis B)
```
stream cut at 12:00:00: 4.12 of 8.25 GB
single [fast] 132659542 book msgs (134508460 total) in 16.260 s = 122.6 ns per book msg, pinned yes
stream cut at 12:00:00: 4.12 of 8.25 GB
single [fast, timed] 132659542 book msgs (134508460 total) in 29.144 s = 219.7 ns per book msg, pinned yes
  book update (t2-t1): mean 168.6 ns  p50 153  p90 333  p99 600  p99.9 906 ns
```

### perf_pipeline_b1
```
 Performance counter stats for '/actions-runner/_work/itch_feed_handler/itch_feed_handler/build-bench/feed_handler bench data/12302019.NASDAQ_ITCH50 --until 12:00:00 --runs 1 --warmup 0 --cpu-producer 1 --cpu-consumer 2 --out results/diagnose/perf_tmp':
          70904.54 msec task-clock                                                            
               194      context-switches                                                      
                 2      cpu-migrations                                                        
             69654      page-faults                                                           
      228132873600      cycles                                                                
       87278903369      instructions                                                          
         637885235      branch-misses                                                         
                 0      cache-references                                                      
                 0      cache-misses                                                          
         738191481      L1-dcache-load-misses                                                 
   <not supported>      LLC-load-misses                                                       
          18731848      dTLB-load-misses                                                      
      36.119553558 seconds time elapsed
      70.768564000 seconds user
       0.135986000 seconds sys
```

### perf_pipeline_b32
```
 Performance counter stats for '/actions-runner/_work/itch_feed_handler/itch_feed_handler/build-bench/feed_handler bench data/12302019.NASDAQ_ITCH50 --until 12:00:00 --runs 1 --warmup 0 --cpu-producer 1 --cpu-consumer 2 --ring-batch 32 --out results/diagnose/perf_tmp':
          65035.68 msec task-clock                                                            
               139      context-switches                                                      
                 2      cpu-migrations                                                        
             69656      page-faults                                                           
      209156499748      cycles                                                                
       94587001942      instructions                                                          
         515559547      branch-misses                                                         
                 0      cache-references                                                      
                 0      cache-misses                                                          
         564328080      L1-dcache-load-misses                                                 
   <not supported>      LLC-load-misses                                                       
          18999008      dTLB-load-misses                                                      
      33.186970570 seconds time elapsed
      64.893164000 seconds user
       0.142968000 seconds sys
```

### perf_single
```
 Performance counter stats for '/actions-runner/_work/itch_feed_handler/itch_feed_handler/build-bench/feed_handler single data/12302019.NASDAQ_ITCH50 --until 12:00:00 --cpu 2':
          17496.05 msec task-clock                                                            
                96      context-switches                                                      
                 1      cpu-migrations                                                        
             69313      page-faults                                                           
       56384462800      cycles                                                                
       42041463087      instructions                                                          
         424684841      branch-misses                                                         
                 0      cache-references                                                      
                 0      cache-misses                                                          
         509587352      L1-dcache-load-misses                                                 
   <not supported>      LLC-load-misses                                                       
          19160692      dTLB-load-misses                                                      
      17.502673430 seconds time elapsed
      17.376765000 seconds user
       0.120213000 seconds sys
```
