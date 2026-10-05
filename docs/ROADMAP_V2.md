# Version 2: from replay tool to production-shaped feed handler

Version 1 (Stages A and B) proves the core: a pinned two-thread pipeline that
rebuilds every book from a full NASDAQ day, a lock-free ring that beats the
industry baselines, and correctness checked after every message. It reads a
**file**, measures **parse-to-book** latency, and keeps an **aggregated** book.
A trading firm's feed handler reads the **network**, is judged on
**wire-to-book** latency, and often needs **order-level** books.

Version 2 closes those three gaps, in order of value.

## The idea in one analogy

Version 1 sorts mail that someone already carried into the building. Version 2
stands at the loading dock. Trucks arrive on two roads at once, some letters
get lost on the way, and the clock starts the moment a truck pulls in, not
when the sack is opened.

## Goals

| Goal | Version 1 | Version 2 target |
|---|---|---|
| Input | memory-mapped file | UDP multicast (MoldUDP64), two redundant lines |
| Gaps | impossible (a file has none) | detected, recovered, and verified |
| Latency measured | parse stamp → book updated | NIC receive timestamp → book updated (wire-to-book) |
| Book | aggregated price levels | order-level, FIFO queue position at every level |
| Hot-path allocation | possible (level vectors grow, order map can rehash) | none, proven by an allocation counter |
| Correctness | golden model after every message | the same, over the network path, including after recovery |

## Component 1. Network replay and A/B line arbitration

| Piece | What it does |
|---|---|
| `itch_publisher` | Replays the ITCH day as **MoldUDP64** packets (session, sequence number, message count, then length-prefixed messages) to two multicast groups, line A and line B, at a configurable rate or following the ITCH timestamps |
| Fault injection | Drops, duplicates, reorders and delays packets per line by configurable rates (for example 0.1% loss on A, 0.1% on B, never the same packet on both) |
| Receiver | One thread per line reads packets (`recvmmsg` first, kernel bypass in Component 4). An **arbiter** takes each sequence number from whichever line delivers it first and drops the duplicate |
| Re-request server | MoldUDP64 retransmission: the receiver sends a UDP re-request (session, sequence number, count) and gets the missing messages back, so gaps lost on *both* lines can be recovered |
| Snapshot | Late-join and unrecoverable gaps resync from a book snapshot (the GLIMPSE model, which Nasdaq serves over SoupBinTCP): load the snapshot, then apply only messages after its sequence number |

| Design choice | Why |
|---|---|
| Arbitrate on the MoldUDP64 sequence number, not arrival order | The two lines take different network paths; the first copy to arrive wins, with no double application |
| A bounded gap buffer while waiting for recovery | Messages after a gap are held, not applied, so the book never sees them out of order |
| Gap counters and recovery latency in the report | A feed handler that silently stalls on a gap is worse than one that is slow |

## Component 2. Order-level book with queue position, zero allocation

| Structure | Version 1 | Version 2 |
|---|---|---|
| Level contents | total shares + order count | an intrusive FIFO list of orders, so every order knows its queue position |
| Orders | open-addressing map of order state | the same map, pointing into a preallocated **order pool** (free list, no `new`) |
| Price levels | sorted vector per side | a **price-indexed array** around the touch (O(1) level lookup), plus a fallback tree for deep levels, all from preallocated pools |
| Capacity | grows if needed | sized from the measured peak (1.92 M live orders on 2019-12-30) plus headroom; overflow is a counted, reported error |

New queries: queue position and shares ahead of a given order; top-N levels
with their order lists. A global `operator new` hook counts allocations, and
the hot path must report **zero** after warmup.

## Component 3. Wire-to-book latency with hardware timestamps

| Piece | How |
|---|---|
| Receive timestamp | `SO_TIMESTAMPING` with hardware RX timestamps where the NIC and driver support them. Support on EC2 varies by instance type and ENA driver version, so check with `ethtool -T` on the chosen instance before relying on it; a software timestamp (`SO_TIMESTAMPNS`) is the measured fallback |
| Clock alignment | Convert NIC (PHC) time to the TSC once per second, so stamps from the two clocks are comparable; report the conversion error |
| Points | NIC receive → packet decoded → message in ring → book updated, so each stage is visible |
| Honest reporting | Keep sampled timing and the coordinated-omission correction from Version 1; report gaps and recoveries next to the latency they caused |

## Component 4. Kernel bypass and host tuning

| Piece | How |
|---|---|
| AF_XDP receive | Packet receive into user space through an XDP socket, bypassing most of the kernel network stack. Whether the ENA driver runs it zero-copy depends on the driver and kernel version, so measure it; `recvmmsg` stays as the baseline it must beat |
| DPDK (optional) | The ENA poll-mode driver, for a second kernel-bypass comparison |
| Host | `isolcpus` / `nohz_full` for the pinned cores, IRQ affinity away from them, huge pages for the mapping, the order pool and the ring, the `performance` cpufreq governor, NUMA-local memory, all checked and printed with each result |

## Component 5. Production hardening

| Piece | Why |
|---|---|
| Counters exported once per second (messages, gaps, recoveries, ring depth, latency percentiles) | A feed handler must be watched while it runs |
| Session handling | Start of day, end of day, session change, and restart mid-day from a snapshot |
| No exceptions or allocation after warmup, checked in CI | Predictable latency |
| Deterministic replay of any captured session, bit for bit | Every production incident becomes a reproducible test |

## Testing

| Test | What it proves |
|---|---|
| MoldUDP64 encode/decode unit tests | Packet framing, sequence numbers and message counts are exact |
| Arbitration under fault injection | With random loss, duplication and reordering on both lines, the applied sequence is exactly 1, 2, 3 … and the book hash matches the golden model at every checkpoint |
| Gap recovery | Gaps on both lines are recovered from the re-request server; unrecoverable gaps trigger a snapshot resync, and the book still matches afterwards |
| Queue position | Order-level book vs a reference that replays every order's FIFO history; queue positions match after every message |
| Zero allocation | The allocation counter reads 0 across the timed section of a full-day replay |
| Everything from Version 1 | Stress, sanitizers, and the deliberate-bug check keep running in CI |

## Benchmark method

The publisher and receiver run on **two separate bare-metal hosts** in one EC2
cluster placement group, so packets cross a real network. Report wire-to-book
p50 / p90 / p99 / p99.9 / max at a fixed rate and following the ITCH
timestamps, with and without injected loss, for `recvmmsg` vs AF_XDP. Record
the NIC, driver, kernel, tuning and timestamp source with every result, as in
Version 1.

## Build plan

| Work | Done when |
|---|---|
| MoldUDP64 publisher + single-line receiver on localhost | Full day received over UDP; the book matches the golden model at every checkpoint |
| Two lines + arbiter + fault injection | Zero mismatches with random loss, duplication and reordering on both lines |
| Re-request server + snapshot resync | Every injected gap recovered or resynced; book still matches |
| Order-level book with pools and a price-indexed array | Queue positions match the reference; allocation counter 0 after warmup |
| Hardware RX timestamps and PHC→TSC conversion | Wire-to-book percentiles reported, with the conversion error |
| AF_XDP receive path | Measured against `recvmmsg` on two bare-metal hosts |
| Host tuning, counters, session handling | Results table includes the tuning; counters exported live |
| README and design doc | Version 2 results below Version 1, with the trade-offs explained |

## Deliverables

1. Publisher, receiver, arbiter, re-request server and snapshot resync, all tested under fault injection.
2. Order-level book with queue position and a zero-allocation hot path.
3. Wire-to-book latency tables and charts, `recvmmsg` vs AF_XDP, with and without loss.
4. CI covering the new tests and the full Version 1 suite.
5. Design doc sections on arbitration and recovery, the order-level book, timestamping, and kernel bypass.

## Cost and prerequisites

* Two bare-metal EC2 hosts in a cluster placement group for the network
  benchmark (several dollars per hour each, for a run of about an hour).
  Everything before that develops on one machine over loopback.
* Hardware RX timestamps and zero-copy AF_XDP depend on instance type,
  driver and kernel. Both are checked on the chosen hosts before the
  benchmark; where they are not available, the fallback is measured and the
  results say so.
* The vCPU quota (now 192) already covers two `c7i.metal-24xl` hosts.
