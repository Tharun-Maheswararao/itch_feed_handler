## Publishing overhead: Intel(R) Xeon(R) Platinum 8488C, Linux 7.0.0-1014-aws x86_64

Every run, so the spread is visible (book msgs/s; end-to-end p99 for paced):

* unpaced off: 7.98 M, 8.08 M, 8.10 M, 8.20 M, 8.14 M, 8.17 M
* unpaced on: 8.03 M, 7.95 M, 7.97 M, 8.10 M, 8.13 M, 8.10 M
* paced off: 2.00 M / 3.0 us, 2.00 M / 3.0 us, 2.00 M / 3.0 us, 2.00 M / 3.2 us, 2.00 M / 3.0 us, 2.00 M / 3.0 us
* paced on: 2.00 M / 3.1 us, 2.00 M / 3.3 us, 2.00 M / 3.3 us, 2.00 M / 3.4 us, 2.00 M / 3.3 us, 2.00 M / 3.1 us

| Replay | Publishing | Runs | Book msgs/s (median) | p50 | p99 | p99.9 |
|---|---|---:|---:|---:|---:|---:|
| unpaced | off | 6 | 8.12 M | backlog | backlog | backlog |
| unpaced | on | 6 | 8.06 M | backlog | backlog | backlog |
| paced | off | 6 | 2.00 M | 0.45 us | 3.04 us | 8.75 us |
| paced | on | 6 | 2.00 M | 0.45 us | 3.25 us | 9.07 us |

Subscriber (per invocation):
  received 761 snapshots, missed 0 (sequence gaps after the first), 148 bytes mean; send->receive median 502 us, p99 779 us
  received 761 snapshots, missed 0 (sequence gaps after the first), 148 bytes mean; send->receive median 481 us, p99 757 us
  received 192 snapshots, missed 0 (sequence gaps after the first), 148 bytes mean; send->receive median 424 us, p99 797 us
  received 189 snapshots, missed 0 (sequence gaps after the first), 148 bytes mean; send->receive median 614 us, p99 907 us
