## Publishing overhead: Apple M4, Darwin 24.6.0 arm64

Every run, so the spread is visible (book msgs/s; end-to-end p99 for paced):

* unpaced off: 14.62 M, 14.57 M, 13.67 M, 12.33 M, 13.11 M, 12.61 M
* unpaced on: 13.69 M, 14.20 M, 13.25 M, 12.82 M, 11.98 M, 14.25 M
* paced off: 2.00 M / 34603.0 us, 2.00 M / 58720.3 us, 2.00 M / 28311.6 us, 2.00 M / 360710.1 us, 2.00 M / 90177.5 us, 2.00 M / 33554.4 us
* paced on: 2.00 M / 192938.0 us, 2.00 M / 218103.8 us, 2.00 M / 33030.1 us, 2.00 M / 310378.5 us, 2.00 M / 24641.5 us, 2.00 M / 40894.5 us

| Replay | Publishing | Runs | Book msgs/s (median) | p50 | p99 | p99.9 |
|---|---|---:|---:|---:|---:|---:|
| unpaced | off | 6 | 13.39 M | backlog | backlog | backlog |
| unpaced | on | 6 | 13.47 M | backlog | backlog | backlog |
| paced | off | 6 | 2.00 M | 0.20 us | 46661.63 us | 111149.05 us |
| paced | on | 6 | 2.00 M | 0.21 us | 116916.22 us | 165675.01 us |

Subscriber (per invocation):
  received 729 snapshots, missed 0 (sequence gaps after the first), 148 bytes mean; send->receive median 136 us, p99 2761 us
  received 728 snapshots, missed 0 (sequence gaps after the first), 148 bytes mean; send->receive median 144 us, p99 11965 us
  received 114 snapshots, missed 0 (sequence gaps after the first), 147 bytes mean; send->receive median 144 us, p99 3879 us
  received 110 snapshots, missed 0 (sequence gaps after the first), 147 bytes mean; send->receive median 148 us, p99 2878 us
