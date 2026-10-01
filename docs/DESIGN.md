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

Measured: **10.4 ns per message (96 M msgs/s) on the pinned Linux Xeon** over
the full day in page cache, and about 5 ns per message (170–230 M msgs/s) on an
Apple M4 from page cache. On a full-day file that does not fit in RAM, the
parser runs at SSD speed instead.

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

**Optional index batching** (`--ring-batch K`, default 1): each side
publishes its index every *K* messages instead of every message, so the
other core's copy of that cache line is invalidated *K* times less often. To
keep progress guaranteed, a side always publishes before it would wait
(consumer finds the ring empty, producer finds it full), and the producer
flushes whenever it goes idle, which in paced mode means after every message,
so batching adds no latency there. On the M4, raw two-thread transfer drops
from 36 ns to 3 ns per message at K = 32.

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
`std::map<price, Level>`. It is obviously correct, slow (about 270–350 ns per
message) and never optimized. Every optimization is checked against it.

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

Measured on the full day, single threaded: **82.5 ns against 272 ns per
message (3.3×) on the Linux Xeon**, and 63–67 ns against 274–352 ns (4.1–5.6×)
on the Apple M4. Inside the two-thread pipeline the same update costs
220–280 ns on the Xeon. That gap is open; see §10.

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
* **Sampled timing** (`--sample-every N`, the benchmark uses 16). Two clock
  reads and three histogram updates per message cost about as much as the
  book update itself, so timing every message measures a pipeline running at
  half speed. The producer marks a random 1-in-N subset (xorshift64, so it
  cannot line up with periodic patterns in the feed). Only marked messages
  are stamped and timed, on both threads; the rest carry no instrumentation.
  Each timed message also records the ring depth at that moment, so a
  backlog between samples still shows up. 1 in 16 still gives about 16 M
  samples per day, more than enough for p99.9. The trade-off is that the
  single slowest message can fall between samples; anything that delays the
  messages behind it cannot. `--sample-every 1` times every message, and the
  benchmark runs one such configuration for comparison.
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

1. **Cheaper timestamps for the split.** Sampling (§6) fixed the cost of
   measurement: on the pinned Xeon, unpaced throughput rose from 3.18 to
   7.69 M msgs/s and paced p99 fell from 4.4–5.3 to 3.0–3.3 µs, with p50,
   p99.9 and max unchanged. What remains is precision: `rdtsc` is not
   ordered, so the queue/book split is only good to tens of ns.
   `rdtscp` + `lfence` on the sampled messages only would sharpen it at a
   cost paid 1 time in 16. Ring index batching (`--ring-batch K`) stays off
   by default: +6–10% unpaced throughput, no paced-latency gain
   ([diagnosis](../results/linux/diagnose/DIAGNOSE.md)).
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

---

# Stage B: proving the lock-free pipeline is faster *and* correct

## 11. Memory ordering in the SPSC ring

The ring needs exactly one ordering guarantee: **when the consumer sees the
new head index, it must also see the slot the producer wrote before
advancing it.** Everything else can be relaxed.

```
producer                                   consumer
slots_[h & mask] = msg;        (plain)     t == head_cache_ ? head_cache_ = head_.load(acquire)
head_.store(h + 1, release);   ──────────► out = slots_[t & mask];   (plain)
                                            tail_.store(t + 1, release) ──► producer's tail_.load(acquire)
```

* `store(release)` on the index: no earlier write (the slot) may be
  reordered after it.
* `load(acquire)` on the other side: no later read (the slot) may be
  reordered before it. A release store that is read by an acquire load
  creates a *happens-before* edge, so the slot write is visible.
* The same pair runs the other way on `tail_`: the producer must not
  overwrite a slot until the consumer has finished reading it.
* Each side reads its *own* index with `relaxed`, because only it writes it.

What it costs: on x86-64 a release store and an acquire load are ordinary
`mov`s, because x86 (TSO) never reorders a store with an earlier store or a
load with a later load. On ARMv8 they are `stlr` and `ldapr`, real
instructions with real ordering cost. That asymmetry is what §13 exploits.

## 12. False sharing

`spsc_unaligned` is the same queue with `kPadded = false`. The producer's
index, its cached copy of the tail, the consumer's index and its cached copy
of the head then share one or two cache lines. Every producer store to `head_`
invalidates the line the consumer is polling for `tail_`, and vice versa,
even though the two never touch each other's variables. This is *false*
sharing, because the sharing is an accident of layout. Padding each field to
its own line (128 B on Apple Silicon, 64 B elsewhere) removes it.

Measured on the pinned Xeon, 64K slots
([chart](../docs/matrix/c7i.2xlarge/alignment.png)):

| Placement | Padded | Unpadded | |
|---|---:|---:|---|
| cross-core, unpaced throughput | 7.10 M/s | 4.90 M/s | **−31%** |
| cross-core, paced p99 | 21.3 µs | 1.01 ms | **47×** |
| same-core, unpaced throughput | 6.60 M/s | 6.09 M/s | −8% |

On the two hyperthreads of one core, the "other core" shares the same L1, so
an invalidation costs almost nothing and the penalty nearly disappears. False
sharing is a cross-core cost, which is why it never shows up in a
single-machine, unpinned benchmark that happens to schedule both threads
close together.

## 13. The deliberate bug

**The change:** one line, behind `-DFH_INJECT_ORDERING_BUG=ON`. The producer
publishes the head with `memory_order_relaxed` instead of `release`
(`SpscQueue::publish_head`). Everything else is identical.

**1. ThreadSanitizer reports it immediately**
([report](../results/bug_experiment/tsan_report.txt)): *"data race … Read of
size 8 … by thread T1"* (the consumer reading a slot) against *"Previous write
of size 8 … by main thread"* (the producer writing that slot). Without the
release/acquire pair there is no happens-before edge between them, so under
the C++ memory model the program has a data race and undefined behaviour.
CI rebuilds the bug on every push, and the `deliberate-bug` job passes only
if TSan still catches it.

**2. On ARM it corrupts real data.** A release build with the bug on an
Apple M4, through `queue_stress`, which checks every field of every 48-byte
message ([log](../results/bug_experiment/runtime_arm64_m4.txt)):

| Ring | Messages | Out of sequence | Corrupted payload |
|---:|---:|---:|---:|
| 4 slots | 2.78 billion | 2,572,990 | 1,338,995 |
| 64 slots | 14.8 billion | 2,933,300 | 3,324,560 |
| 1024 slots | 17.4 billion | 2,271,726 | 2,287,864 |
| correct build, 4 and 64 slots (control) | 9.6 billion | **0** | **0** |

The buggy build was also *faster* (246 against 130 M ops/s at 64 slots),
because `str` skips the ordering work that `stlr` does. A benchmark alone
would have rewarded the bug.

**3. On x86 it cannot show up with this compiler.** The generated code
([asm](../results/bug_experiment/asm/)) for one `try_push`:

| | correct (`release`) | bug (`relaxed`) |
|---|---|---|
| ARM64 | slot: `stp q1,q2,[x8,#16]`; `str q0,[x8]`; then index: **`stlr x9,[x0]`** | the same slot stores, then **`str x9,[x0]`** |
| x86-64 | slot: three `movaps`; then index: `movq %rax,(%rdi)` | **identical, byte for byte** |

In both ARM builds the compiler keeps the slot stores *before* the index
store. The reordering that corrupted millions of messages is done by the
CPU: ARM lets a later store become visible to another core before an
earlier one, and `stlr` is what forbids that. x86 never reorders
store-store, so a release store *is* a plain store, and the machine code does
not change. The bug is still real on x86. The C++ standard allows the
compiler to sink the slot write below a relaxed store, and a different
compiler version, optimisation level or surrounding code could do so. Only a
tool that checks the *language* rules (TSan) finds it reliably.

**4. The x86 runtime run confirms it.** The same buggy release build, pinned
on the Xeon ([log](../results/bug_experiment/runtime_x86_c7i.2xlarge.txt)):
4, 64 and 1024 slots, 60 s each, **1.09 billion messages, 0 sequence errors,
0 payload errors.** The identical source corrupts millions of messages on ARM
and none on x86.

**The lesson:** a test suite that passes on x86 says nothing about memory
ordering. Run the stress tests on ARM, run TSan, and treat every
`memory_order` argument as a claim that needs a reason.

## 14. Design comparison: Rigtorp and Boost

I read both implementations only after this repo's ring had been designed
and benchmarked in Stage A, so mine could not drift toward theirs. Each sits
behind a thin adapter ([`industry_queues.hpp`](../include/fh/queue/industry_queues.hpp))
that uses only its public API and waits with the same `cpu_relax()` spin.

| | This repo | Rigtorp SPSCQueue (`1053918`) | boost::lockfree::spsc_queue (1.83 / 1.92) |
|---|---|---|---|
| Capacity | compile-time power of two | runtime, any size | compile-time (as used here) |
| Index wrap | `i & mask` on 64-bit indices that never wrap | compare with capacity, reset to 0 | `& mask` only if *N+1* is a power of two, else a subtract loop |
| Full vs empty | `head − tail == N`, all N slots usable | one slack slot (N+1 allocated) | one slack slot (N+1 allocated) |
| Cached copy of the other index | yes, refreshed only when it looks full or empty | yes, same idea | **no**: every push loads `read_index_`, every pop loads `write_index_` |
| Index padding | each index and each cache on its own line; 128 B on Apple Silicon | each on its own line; `hardware_destructive_interference_size`, else 64 B | the two indices padded to a per-architecture size (64 / 128 / 256 B) |
| Slot array | cache-line aligned, not padded at the ends | padded with `kPadding` slots at both ends against neighbouring heap data | stored inside the queue object (compile-time capacity) |
| Read API | copy out (`try_pop(T&)`) | zero-copy (`front()` then `pop()`) | copy out via a functor (`consume_one`) |
| Ordering | relaxed own / acquire other / release publish | identical | identical |
| Extras | optional index batching (`set_batch`) | allocator support, `emplace` | bulk push/pop, iterator ranges |

**Why Boost is slower across cores.** Without a cached index, every
operation reads the other core's cache line. While the producer is writing
`write_index_` once per message, the consumer reads it once per message, so
the line goes back and forth between the cores on almost every operation.
On one core, where the "other core" is the sibling hyperthread on the same
L1, that costs little, and Boost matches the others (6.56 M/s same-core). Across
cores it drops to 5.60 M/s, and in the paced bursts its p99 is 46× this
repo's (983 µs against 21.3 µs). There is also a smaller, avoidable cost here:
`capacity<65536>` allocates 65537 slots, which is not a power of two, so
`next_index` takes the subtract-loop path instead of a mask.
`capacity<65535>` would give Boost its fast path. The matrix keeps the same
nominal capacity for all four queues, as the comparison rules require.

**Rigtorp against this repo: almost the same design, and one difference that
matters.** The memory ordering is identical, both cache the other index, and
both pad each index to its own line. Yet the results depend on where the
threads run:

| Unpaced, 64K slots | same core | different cores | different sockets |
|---|---:|---:|---:|
| this repo, VM (`c7i.2xlarge`) | 6.60 M/s | 7.10 M/s | n/a |
| Rigtorp, VM | 6.73 M/s | 6.08 M/s (−14%) | n/a |
| this repo, bare metal (`c7i.metal-48xl`) | 9.05 M/s | 9.37 M/s | 7.65 M/s |
| Rigtorp, bare metal | 9.30 M/s | 7.89 M/s (−16%) | 4.93 M/s (−36%) |

On one core Rigtorp is slightly *ahead*. The gap appears only when cache
lines travel, and it grows with the distance. That pointed to coherence
rather than instruction count, and `perf c2c` on the bare-metal host,
cross-core, unpaced, 64K slots ([reports](../results/matrix/c7i.metal-48xl/c2c)),
confirms it:

| | this repo | Rigtorp |
|---|---:|---:|
| instructions | 16.7 B | 16.4 B |
| cycles | 32.1 B | **38.3 B (+19%)** |
| loads blocked by data | 366 | **1,110 (3.0×)** |
| sampled loads on the hottest cache line | 3,104 | **6,066 (2.0×)** |
| … loads of that line *by the thread that writes it* | **1** | **101**, 98 of them HITM at ~340 cycles |

The same work takes 19% more cycles, so the difference is stall time. In
both queues one ring index carries almost all the cross-core traffic (83%
and 95% of HITMs): in a full ring the producer is polling the consumer's
index. The difference is what the *owner* of that index does. Rigtorp
re-reads its own index from the shared line at the start of every operation
(`writeIdx_.load(relaxed)` in `try_emplace`, `readIdx_.load(relaxed)` in
`front()` and again in `pop()`). Because the other core is polling that line,
the line has usually just been pulled into the other cache, so the owner's
own-index load becomes a cross-core miss (a HITM), on the critical path,
every time. This ring keeps the owner's copy in a private line
(`Producer::head`, `Consumer::tail`) and only ever *stores* to the shared
line. A store retires into the store buffer and does not stall the thread,
while a load that misses does.

That one choice also explains the pattern across placements. On one core the
"other cache" is the sibling hyperthread's shared L1, so the miss is cheap and
the gap vanishes. Across cores it costs an L2-to-L2 transfer through the mesh
(19%), and across sockets a trip over the socket interconnect (55%). The
other differences in the table above (index representation, slot-array
placement) are not needed to explain it. This is a profile of one
configuration, not a proof that nothing else contributes, but every signal
points the same way.

**What I would take from them.** Rigtorp's end padding of the slot array is
a real improvement this ring lacks: neighbouring heap data can share the
first or last slot's cache line. Its zero-copy `front()`/`pop()` would save a
48-byte copy per message. Boost's bulk `push(begin, end)` is the natural API
for the index batching measured in §10 and Stage A's diagnosis.
