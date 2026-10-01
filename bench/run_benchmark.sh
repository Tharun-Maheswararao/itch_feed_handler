#!/usr/bin/env bash
# One-command benchmark: build, run every configuration, plot the charts.
#
#   bench/run_benchmark.sh data/12302019.NASDAQ_ITCH50
#
# Environment overrides:
#   PRODUCER_CPU / CONSUMER_CPU  cores to pin to (Linux; default: two physical
#                                cores on socket 0, skipping core 0)
#   RATE                         paced replay rate, book msgs/s (default 2000000)
#   UNTIL                        cut paced runs at this ITCH time (default: full
#                                day on Linux; 10:00:00 on macOS so the slice
#                                fits in RAM and SSD paging stays out of the tail)
#   MICRO_REPEAT                 repeats for micro_bench (default 3)
#   RUNS / WARMUP                timed runs and warmup passes (default 5 / 1)
#   VIEW_SYMBOL                  symbol for the view-on run (default AAPL)
#   RESULTS / DOCS               output dirs (default results / docs)
set -euo pipefail

DATA=${1:?usage: bench/run_benchmark.sh ITCH_FILE}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RESULTS=${RESULTS:-$ROOT/results}
DOCS=${DOCS:-$ROOT/docs}
RATE=${RATE:-2000000}
MICRO_REPEAT=${MICRO_REPEAT:-3}
if [[ "$(uname)" == "Darwin" ]]; then UNTIL=${UNTIL-10:00:00}; else UNTIL=${UNTIL-}; fi
CUT=()
if [[ -n "$UNTIL" ]]; then CUT=(--until "$UNTIL"); fi
PACED="paced_$((RATE / 1000000))M"
RUNS=${RUNS:-5}
WARMUP=${WARMUP:-1}
VIEW_SYMBOL=${VIEW_SYMBOL:-AAPL}
BUILD=$ROOT/build-bench

echo "== build (Release, native)"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DFH_NATIVE=ON -DFH_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN)" --target feed_handler micro_bench >/dev/null

PIN=()
if [[ "$(uname)" == "Linux" ]]; then
  if [[ -z "${PRODUCER_CPU:-}" || -z "${CONSUMER_CPU:-}" ]]; then
    # First logical CPU of each distinct physical core on socket 0, skipping
    # core 0 (it takes most interrupts). Hyperthread siblings are never paired.
    mapfile -t CORES < <(lscpu -p=CPU,CORE,SOCKET | grep -v '^#' | awk -F, '$3==0 && !seen[$2]++ {print $1}')
    if (( ${#CORES[@]} >= 3 )); then
      PRODUCER_CPU=${PRODUCER_CPU:-${CORES[1]}}
      CONSUMER_CPU=${CONSUMER_CPU:-${CORES[2]}}
    else
      PRODUCER_CPU=${PRODUCER_CPU:-0}
      CONSUMER_CPU=${CONSUMER_CPU:-1}
    fi
  fi
  PIN=(--cpu-producer "$PRODUCER_CPU" --cpu-consumer "$CONSUMER_CPU")
  echo "== pinning producer to CPU $PRODUCER_CPU, consumer to CPU $CONSUMER_CPU"
  gov=/sys/devices/system/cpu/cpu$CONSUMER_CPU/cpufreq/scaling_governor
  if [[ -r $gov ]]; then echo "   cpufreq governor: $(cat "$gov") (use 'performance' for stable numbers)"; fi
else
  echo "== $(uname): no thread pinning available; numbers are indicative only"
fi

mkdir -p "$RESULTS/logs"
run() {  # label, extra args...
  local label=$1; shift
  echo "== bench: $label"
  "$BUILD/feed_handler" bench "$DATA" --book fast --runs "$RUNS" --warmup "$WARMUP" \
    --label "$label" --out "$RESULTS/$label" ${PIN[@]+"${PIN[@]}"} "$@" | tee "$RESULTS/logs/$label.txt"
}

run unpaced
run "$PACED" --rate "$RATE" ${CUT[@]+"${CUT[@]}"}
run "${PACED}_view" --rate "$RATE" ${CUT[@]+"${CUT[@]}"} --view "$VIEW_SYMBOL" --no-draw \
  --record "$RESULTS/logs/view_frames.txt"

echo "== component micro-benchmarks"
"$BUILD/micro_bench" "$DATA" --repeat "$MICRO_REPEAT" | tee "$RESULTS/logs/micro_bench.txt"

echo "== charts"
python3 "$ROOT/scripts/plot_results.py" --results "$RESULTS" --out "$DOCS" \
  --configs "unpaced,$PACED,${PACED}_view"
echo "== live view GIF (AAPL from the open, 5x real time)"
"$BUILD/feed_handler" bench "$DATA" --runs 1 --warmup 0 --view "$VIEW_SYMBOL" --speedup 5 \
  --pace-from 09:30:00 --until 09:31:15 --view-interval-ms 250 --no-draw \
  --record "$RESULTS/logs/view_frames.txt" --label demo_view --out "$RESULTS/logs/demo_view" >/dev/null
python3 "$ROOT/scripts/make_gif.py" "$RESULTS/logs/view_frames.txt" "$DOCS/live_book.gif" --ms 250
echo "done: CSVs in $RESULTS, charts in $DOCS"
