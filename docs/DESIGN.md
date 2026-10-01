# Design notes

This document explains each design decision in the feed handler, the
alternatives considered, how correctness is established, and what I would
change next. Measured numbers are in the [README](../README.md).

## 1. Pipeline shape

```
mmap(file) ──► parser thread ──► SPSC ring (16384 × 48 B) ──► book thread ──► books[locate]
               decode A F E C X D U R          lock free          apply, stamp, histogram
               stamp t0                                           t1 = pop, t2 = applied
```

Two threads, one queue, one direction. The parser does nothing but byte
decoding, so it runs over 10× faster than the book can absorb messages. The
ring decouples them, so a slow book update (a deep-level insert, a cache miss
in the order map) does not stall decoding, and the reverse.

**Latency points.** `t0` is stamped by the parser once a message is decoded
(or, in paced mode, set to the scheduled arrival time; see §6). The book
thread stamps `t1` right after popping and `t2` right after the book update.
Three histograms record `t1−t0` (queue wait), `t2−t1` (book update) and their
sum (end to end).

## 2. Parser

* **Memory mapped**, `MADV_SEQUENTIAL | MADV_WILLNEED`. The parser walks one
  pointer through the mapping: read the 2-byte length, switch on the type
  byte, decode, advance. There are no copies, no read-buffer management, and
  no chunk-boundary cases.
* **Byte order.** Every multi-byte field is `memcpy` into an integer and then
  `__builtin_bswap*`. That compiles to an unaligned load plus `rev`/`bswap`,
  and it is well-defined C++ (no type-punned pointer casts).
  The 6-byte timestamp is assembled from a 2-byte and a 4-byte load so it
  never reads past the message, which matters for the last message in the map.
* **Only what the book needs** is decoded. Every other type is one increment
  in a 256-entry count array plus a pointer advance. The length prefix is
  trusted for framing; each decoded type is also checked against its spec
  length and counted as malformed if it is short (none in the real file).
* **Message struct: 48 bytes**, `alignas(16)`. 32 bytes is possible by bit
  packing (side into the top bit of price, type into spare bits) or by
  dropping the ITCH timestamp. I kept the timestamp because paced replay and
  the live view use it, and chose clarity over the extra packing. See §10.

Measured: about **5 ns per message, 170–230 M msgs/s, 5–7 GB/s** when the data is
in the page cache. On a full-day file that does not fit in RAM, the parser runs
at SSD speed instead.

## 3. SPSC ring buffer

| Decision | Why |
|---|---|
| Power-of-two capacity, template constant | `index & mask`, folded to an immediate |
| 64-bit monotonic `head`/`tail` | full is `head − tail == N`, empty is `head == tail`; no wasted slot, no ABA |
| Producer owns `head`, consumer owns `tail` | each index has exactly one writer, so no RMW atomics, only loads and stores |
| `store(release)` on publish, `load(acquire)` on observe | the consumer that sees the new head also sees the slot written before it |
| Each index and each cached copy on its **own cache line** | no false sharing; padding is 128 B on Apple Silicon (its real line size), 64 B elsewhere |
| Cached copy of the other side's index | the producer rereads `tail` only when it *thinks* the ring is full, the consumer rereads `head` only when it thinks it is empty. In steady state each side touches only its own lines. |
| Busy spin with `pause`/`yield` | lowest wake-up latency, at the cost of a full core per thread |
| 16384 slots (768 KiB) | fits in L2; absorbs bursts of about 1 ms at the book's service rate |

The end of the stream is a sentinel message (`MsgType::EndOfStream`), so the
consumer needs no second "done" flag and nothing is lost at shutdown.

Tests: tiny (2), small (64) and large (16384) rings with every value checked
in order (tens of millions of items in CI, **2 billion** run locally), full
48-byte payload integrity, wrap-around, and ThreadSanitizer in CI.

## 4. Order book

Books are a `std::vector` indexed directly by the 2-byte stock locate, with
65536 entries allocated up front, so finding a stock needs no hashing. Order
references are day-unique across the whole feed, so there is a single global
order map, not one per stock.

### Golden model (kept forever)

`std::unordered_map<ref, Order>` plus, for each stock, two
`std::map<price, Level>`. It is obviously correct, slow (about 265 ns per
message here) and never optimized. Every optimization is checked against it.

### Optimized book

**Order map:** preallocated open addressing.

* Linear probing over a power-of-two table of 24-byte entries (key 0 = empty).
* Fibonacci hashing (`ref × 2^64/φ >> shift`). ITCH references are roughly
  sequential, and masking the low bits would cluster them badly.
* **Backward-shift deletion** instead of tombstones. This day has 118 M adds
  and 118 M removals, and tombstones would steadily lengthen probe chains.
* Sized at 2^23 slots (192 MiB). Peak live orders on this day is 1.92 M, so the
  load factor stays under 0.23. It grows only above 0.5, and the grow count is
  reported (0 on the real day). On Linux the table is 2 MiB-aligned and
  `MADV_HUGEPAGE`d to cut TLB misses on random probes.

**Price levels:** a sorted `std::vector<Level>` per side, ordered worst to
best so the touch is at the back.

* Almost all activity is within a few levels of the touch. A linear scan from
  the back finds the level in one to three compares, and inserting or erasing
  there moves only the few elements behind it.
* The scan is capped at 8 steps, then falls back to `std::lower_bound`.
  Liquid names carry thousands of levels (AAPL has 3500+ bid levels), mostly
  stub quotes far from the touch.
* Both sides use one ordering by storing `key = price` for bids and `~price`
  for asks, so "ascending key" always means "towards the touch".
* Level = {key, order count, total shares}: 16 bytes, four to a cache line.

Measured on the full day: **about 64 ns against 265 ns per message, 4.1×** over
the golden model.

### Semantics (identical in both books)

| Msg | Action |
|---|---|
| R | locate → symbol |
| A, F | insert at the price level; F's attribution is ignored |
| E, C | reduce shares; remove at zero. C executes at a different price, but the resting order sits at its display price, so the book effect equals E |
| X | reduce shares (removed at zero, defensively) |
| D | remove |
| U | remove the old order, add the new reference with the new price and shares; **side and stock are kept from the original order** (spec 1.4.5) |

Anomalies are counted, never ignored silently: unknown reference, overfill
(execute or cancel larger than the open size), and duplicate add. All three are
**zero** on the real day.

## 5. Proving the optimized pipeline is correct

1. **Running hash of book state.** Each non-empty level contributes
   `mix(locate, side, price, shares, orders)`, and the book hash is the
   wrapping sum of the contributions. Every level change subtracts the old
   contribution and adds the new one, so it costs O(1) and does not depend on
   the order levels are stored in. Both books maintain it, behind a template
   flag so benchmark builds pay nothing.
2. **Per-message chain.** After every message, `digest = mix(digest ^ hash)`.
   Equal final digests mean the two books had equal hashes after *every one* of
   the 263 M messages, not just at the end. Checkpoints every 2^20 messages
   locate the first divergence if there is one.
3. **Full-state comparison.** Every level of every stock, plus counters and
   symbols. The day ends with all orders deleted, so the comparison is also
   run with `--until 12:00:00` against a live book of 1,771,377 orders.
4. **Invariants.** Each level's shares and order count equal the sum of its
   orders, levels are strictly sorted, no order has zero shares, and the live
   count matches. This also proves no order is in two places: each order is in
   exactly one map slot, and the rebuilt aggregates match exactly.
5. **Edge cases**, run against both books in typed tests: a replace that
   moves level, a replace priced through the opposite touch (it stays on its
   side), a replace at the same price, an execution that empties a level, a
   delete of the last order (the hash returns to 0), unknown references,
   overfills, deep books that exercise the binary-search path, and order-map
   churn against `std::unordered_map` with forced wrap-around.
6. **Synthetic feed in CI.** A generator writes a valid random ITCH stream
   with replaces, crosses, unknown references and skipped types. CI runs the
   whole count → golden → verify → bench → plot chain on it, because the real
   file is too large for CI.

## 6. Latency measurement

* **Clock.** `rdtsc` on x86 (invariant TSC, calibrated against
  `steady_clock`), `cntvct_el0` on AArch64 (frequency from `cntfrq_el0`), and
  `steady_clock` as a fallback (`-DFH_USE_CHRONO=ON`). Units and resolution
  are not the same thing. On the Apple M4 the counter reports 1 GHz units but
  only advances in **41.67 ns steps** (a 24 MHz timer scaled up), so a
  60 ns book update reads as one or two steps. The program therefore
  *measures* the resolution at startup and writes it into every result. The
  *mean* book update is also reported from summed busy time, which is accurate
  in aggregate. On x86 with `rdtsc` the resolution is under 1 ns.
* **Histogram.** Log-linear (HDR-style): exact below 32 ticks, then 32
  linear sub-buckets per power of two (≤ 3.1% relative error), 1440 `uint64_t`
  buckets in a fixed array. `record()` is a `clz`, a shift and an increment,
  with no allocation. Percentiles report the bucket's upper edge, so they
  never under-report.
* **Two replay modes, because they answer different questions.**
  * *Unpaced* replays as fast as possible. That measures throughput, but the
    parser outruns the book, so the ring is always full and every message
    waits about `capacity × service time` (about 1–2 ms). Queue wait in this
    mode is backlog, not a property of the code.
  * *Paced* (`--rate N`) releases messages on a fixed schedule below the book's
    capacity, the way a real feed arrives. This is the latency number that
    means something. The benchmark uses 2 M msgs/s, about 30% of book
    capacity. On macOS it replays 04:00–10:00 (`--until`), which includes the
    open and fits in RAM, so SSD paging stays out of the tail. On Linux it
    replays the full day.
* **Coordinated omission.** In paced mode the stamp is the *scheduled* arrival
  time, not the time the producer got round to the message. If the producer
  stalls (a full ring, a page fault on the mapping), the delay is charged to
  the messages that were delayed instead of disappearing from the histogram.

## 7. Benchmark method

The benchmark is Linux only, because macOS cannot pin threads. Each thread is
pinned to its own physical core (not core 0, never two hyperthread siblings);
`bench/run_benchmark.sh` picks the cores from `lscpu`. The build uses `-O3
-march=native`. Each configuration gets one warmup pass and five timed runs,
and the median by throughput is reported. CPU model, core counts, compiler,
flags and clock source are written into every CSV row. Each run builds fresh
books (the 192 MiB order table is zeroed, which also pre-faults it) outside the
timed region.

## 8. Live view and the seqlock

The view is off by default. When enabled (`--view AAPL`), the book thread
copies the top five levels of one stock into a seqlock every 4096 messages.
That is ten 16-byte levels, and the writer never blocks. The view thread wakes
once per second, reads the snapshot, retries if it caught a half-written copy,
and redraws the terminal with ANSI clear and home.

The payload is stored as relaxed `std::atomic<uint64_t>` words, bracketed by
a release fence and a release store on the writer side, and an acquire load
and an acquire fence on the reader side (Boehm 2012). Concurrent reads are
therefore well-defined C++ rather than a "benign" data race. The view-on and
view-off benchmarks are run back to back to show the pipeline is unaffected.
A recorded frame log becomes the README GIF through `scripts/make_gif.py`,
with no screen recorder involved.

## 9. Things deliberately not done

* No allocation-free golden model, no micro-tuning of it: its job is to be
  obviously right.
* No multi-day state (each file starts from an empty book, as the feed does).
* No MoldUDP64 / SoupBinTCP session layer: the sample file is the raw
  length-prefixed stream.

## 10. What I would change next

1. **Batching across the ring.** Publish `head` once per *k* messages, and
   pop runs of messages with a single acquire. This trades a few ns of
   latency for fewer cache-line transfers, and would lift the SPSC ceiling
   (18–34 ns per message on M4 without pinning) well clear of the book.
2. **32-byte messages** by bit-packing side, type and locate. That puts two
   messages per 64-byte line and halves ring bandwidth.
3. **Store a level handle in the order.** A price-indexed array around the
   touch (a ring of levels indexed by `price − base`) gives O(1) level lookup
   and removes the remaining search on every execute, cancel and delete.
4. **Prefetch the order slot.** The parser knows the reference before the
   book does, so it could issue a prefetch into a shared LLC hint, or the
   book thread could prefetch slot `i+4` while applying slot `i`.
5. **Huge pages for the mapping** (`MAP_HUGETLB` with a tmpfs copy), and
   `mlock` so a run never touches the SSD.
6. **Kernel-bypass input** (AF_XDP / DPDK) and a MoldUDP64 gap-fill path, to
   turn this replay tool into a live feed handler.
7. **Per-stock sharding** across several book threads (locate mod N), one ring
   each, once a single book thread is the bottleneck.
