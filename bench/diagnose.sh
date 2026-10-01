#!/usr/bin/env bash
# Diagnose why the two-thread pipeline costs more per message than its parts.
#
#   bench/diagnose.sh data/12302019.NASDAQ_ITCH50
#
# Hypotheses and the experiment for each:
#   A  cross-core cache-line traffic on the ring
#        -> ring index batching (K = 1, 8, 32), and both threads on the two
#           hyperthreads of ONE core (data moves through a shared L1/L2)
#   B  the single-threaded figure is flattered by out-of-order overlap
#        -> single-threaded book with the pipeline's exact per-message clock
#           reads (--timed), and the pipeline with timing off (--no-latency)
#   C  memory / TLB latency on the order table
#        -> perf stat hardware counters (cache, LLC, dTLB misses, IPC)
#
# Environment: UNTIL (default 12:00:00, about half the day), RUNS (default 3),
# OUT (default results/diagnose).
set -euo pipefail

DATA=${1:?usage: bench/diagnose.sh ITCH_FILE}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/results/diagnose}
UNTIL=${UNTIL:-12:00:00}
RUNS=${RUNS:-3}
BUILD=$ROOT/build-bench
LOGS=$OUT/logs
mkdir -p "$LOGS"

echo "== build (Release, native)"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DFH_NATIVE=ON -DFH_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN)" --target feed_handler micro_bench >/dev/null
FH=$BUILD/feed_handler

# Cores: two distinct physical cores (skip core 0), plus the hyperthread
# sibling of the first one, if the machine has SMT.
P=-1; C=-1; SIB=""
if command -v lscpu >/dev/null; then
  mapfile -t CORES < <(lscpu -p=CPU,CORE,SOCKET | grep -v '^#' | awk -F, '$3==0 && !seen[$2]++ {print $1}')
  if (( ${#CORES[@]} >= 3 )); then P=${CORES[1]}; C=${CORES[2]}; fi
  if (( P >= 0 )); then
    core_of_p=$(lscpu -p=CPU,CORE | grep -v '^#' | awk -F, -v p="$P" '$1==p {print $2}')
    SIB=$(lscpu -p=CPU,CORE | grep -v '^#' | awk -F, -v c="$core_of_p" -v p="$P" '$2==c && $1!=p {print $1; exit}')
  fi
fi
echo "== producer CPU $P, consumer CPU $C, SMT sibling of $P: ${SIB:-none}"
lscpu > "$LOGS/lscpu.txt" 2>/dev/null || true

PERF=""
if command -v perf >/dev/null && perf stat -e cycles true >/dev/null 2>&1; then
  PERF="perf stat -e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions,branch-misses,cache-references,cache-misses,L1-dcache-load-misses,LLC-load-misses,dTLB-load-misses"
  echo "== perf available"
else
  echo "== perf not available: skipping hardware counters"
fi

bench() {  # label producer consumer extra-args...
  local label=$1 p=$2 c=$3; shift 3
  echo "== bench $label"
  "$FH" bench "$DATA" --book fast --until "$UNTIL" --runs "$RUNS" --warmup 1 \
    --cpu-producer "$p" --cpu-consumer "$c" --label "$label" --out "$OUT/$label" "$@" > "$LOGS/$label.txt"
  grep -E "^Run" "$LOGS/$label.txt" || true
}

# B: single thread, untimed vs timed with the pipeline's own clock reads
echo "== single-threaded book (hypothesis B)"
{
  "$FH" single "$DATA" --until "$UNTIL" --cpu "$C"
  "$FH" single "$DATA" --until "$UNTIL" --cpu "$C" --timed
} 2>&1 | tee "$LOGS/single.txt"

# A + B: pipeline across two physical cores
bench unpaced_b1        "$P" "$C"
bench unpaced_b1_nolat  "$P" "$C" --no-latency
bench unpaced_b8        "$P" "$C" --ring-batch 8
bench unpaced_b32       "$P" "$C" --ring-batch 32
bench unpaced_b32_nolat "$P" "$C" --ring-batch 32 --no-latency

# A: same pipeline on the two hyperthreads of one physical core
if [[ -n "$SIB" ]]; then
  bench unpaced_b1_smt  "$P" "$SIB"
  bench unpaced_b32_smt "$P" "$SIB" --ring-batch 32
fi

# Latency at 2M msgs/s, with and without batching
bench paced_2M_b1  "$P" "$C" --rate 2000000
bench paced_2M_b32 "$P" "$C" --rate 2000000 --ring-batch 32

# Raw ring cost at each batch size, pinned
echo "== micro_bench"
"$BUILD/micro_bench" "$DATA" --repeat 1 --cpu-producer "$P" --cpu-consumer "$C" | tee "$LOGS/micro_bench.txt"

# C: hardware counters, one run each
if [[ -n "$PERF" ]]; then
  echo "== perf stat"
  $PERF -o "$LOGS/perf_single.txt" -- "$FH" single "$DATA" --until "$UNTIL" --cpu "$C" >/dev/null
  $PERF -o "$LOGS/perf_pipeline_b1.txt" -- "$FH" bench "$DATA" --until "$UNTIL" --runs 1 --warmup 0 \
    --cpu-producer "$P" --cpu-consumer "$C" --out "$OUT/perf_tmp" >/dev/null
  $PERF -o "$LOGS/perf_pipeline_b32.txt" -- "$FH" bench "$DATA" --until "$UNTIL" --runs 1 --warmup 0 \
    --cpu-producer "$P" --cpu-consumer "$C" --ring-batch 32 --out "$OUT/perf_tmp" >/dev/null
  rm -rf "$OUT/perf_tmp"
fi

# Report
PY=python3
for c in ${PYTHON:+"$PYTHON"} python3 /usr/bin/python3; do
  if "$c" -c "import pandas" >/dev/null 2>&1; then PY=$c; break; fi
done
{
  "$PY" "$ROOT/scripts/summarize_results.py" --results "$OUT" --docs "(none)" --title "Pipeline diagnosis (until $UNTIL)"
  echo
  echo "### Single-threaded book (hypothesis B)"
  echo '```'; cat "$LOGS/single.txt"; echo '```'
  for f in "$LOGS"/perf_*.txt; do
    [[ -e "$f" ]] || continue
    echo
    echo "### $(basename "$f" .txt)"
    echo '```'; grep -vE '^\s*$|^#' "$f"; echo '```'
  done
} > "$OUT/DIAGNOSE.md"
echo "== wrote $OUT/DIAGNOSE.md"
