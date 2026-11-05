#!/usr/bin/env bash
# What does the snapshot publisher cost the pipeline? (Version 1.1)
#
#   bench/publish_overhead.sh data/12302019.NASDAQ_ITCH50
#
# Runs the same replay with publishing off and on, alternating off/on/on/off
# so drift (thermal, battery, page cache) hits both sides equally:
#   unpaced  throughput, as fast as the parser can go
#   paced    end-to-end p99 at RATE book msgs/s
# "On" means the book thread writes the snapshot, the publisher thread
# serializes and sends it every 100 ms, and a Python subscriber is connected.
#
# Environment: RATE (default 2000000), UNTIL (default 10:00:00, a slice that
# fits in RAM), RUNS per invocation (default 3), RESULTS (default
# results/publish), BUILD (default build-pub), PRODUCER_CPU/CONSUMER_CPU (Linux).
set -euo pipefail

DATA=${1:?usage: bench/publish_overhead.sh ITCH_FILE}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RATE=${RATE:-2000000}
UNTIL=${UNTIL-10:00:00}
RUNS=${RUNS:-3}
RESULTS=${RESULTS:-$ROOT/results/publish}
BUILD=${BUILD:-$ROOT/build-pub}
PORT=5571

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DFH_ENABLE_PUBLISH=ON -DFH_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN)" --target feed_handler >/dev/null

CUT=()
if [[ -n "$UNTIL" ]]; then CUT=(--until "$UNTIL"); fi
PIN=()
if [[ "$(uname)" == "Linux" ]]; then
  # As in run_benchmark.sh: two distinct physical cores on socket 0, not core 0.
  mapfile -t CORES < <(lscpu -p=CPU,CORE,SOCKET | grep -v '^#' | awk -F, '$3==0 && !seen[$2]++ {print $1}')
  if (( ${#CORES[@]} >= 3 )); then
    PRODUCER_CPU=${PRODUCER_CPU:-${CORES[1]}}; CONSUMER_CPU=${CONSUMER_CPU:-${CORES[2]}}
  fi
  PIN=(--cpu-producer "${PRODUCER_CPU:-0}" --cpu-consumer "${CONSUMER_CPU:-1}")
  echo "== pinning producer to CPU ${PRODUCER_CPU:-0}, consumer to CPU ${CONSUMER_CPU:-1}"
else
  echo "== $(uname): no thread pinning; tail latency is not comparable run to run"
fi
mkdir -p "$RESULTS/logs"

run() {  # mode(off|on) kind(unpaced|paced) pass
  local mode=$1 kind=$2 pass=$3 extra=() sub=
  [[ $kind == paced ]] && extra+=(--rate "$RATE")
  if [[ $mode == on ]]; then
    extra+=(--publish "tcp://127.0.0.1:$PORT" --publish-symbol AAPL)
    python3 "$ROOT/scripts/sub_snapshots.py" AAPL --endpoint "tcp://127.0.0.1:$PORT" --quiet \
      > "$RESULTS/logs/sub_${kind}_$pass.txt" 2>&1 &
    sub=$!
  fi
  echo "== $kind, publishing $mode (pass $pass)"
  "$BUILD/feed_handler" bench "$DATA" --runs "$RUNS" --warmup 1 --sample-every 16 ${CUT[@]+"${CUT[@]}"} \
    ${PIN[@]+"${PIN[@]}"} ${extra[@]+"${extra[@]}"} --label "${kind}_$mode" --out "$RESULTS/${kind}_${mode}_$pass" \
    > "$RESULTS/logs/${kind}_${mode}_$pass.txt"
  grep -E "^publisher" "$RESULTS/logs/${kind}_${mode}_$pass.txt" || true
  if [[ -n $sub ]]; then kill "$sub" 2>/dev/null || true; wait "$sub" 2>/dev/null || true; fi
}

for kind in unpaced paced; do
  run off $kind 1; run on $kind 1; run on $kind 2; run off $kind 2
done

python3 - "$RESULTS" <<'PY' | tee "$RESULTS/SUMMARY.md"
import csv, statistics, sys
from pathlib import Path
res = Path(sys.argv[1])
def rows(kind, mode):
    out = []
    for d in sorted(res.glob(f"{kind}_{mode}_*")):
        out += list(csv.DictReader(open(d / "run_stats.csv")))
    return out
first = rows("unpaced", "off")[0]
print(f"## Publishing overhead: {first['cpu_model']}, {first['os']}\n")
print("Every run, so the spread is visible (book msgs/s; end-to-end p99 for paced):\n")
for kind in ("unpaced", "paced"):
    for mode in ("off", "on"):
        r = rows(kind, mode)
        vals = ", ".join(f"{float(x['book_msgs_per_sec'])/1e6:.2f} M" + (f" / {float(x['total_p99_ns'])/1e3:.1f} us" if kind == "paced" else "") for x in r)
        print(f"* {kind} {mode}: {vals}")
print("\n| Replay | Publishing | Runs | Book msgs/s (median) | p50 | p99 | p99.9 |")
print("|---|---|---:|---:|---:|---:|---:|")
for kind in ("unpaced", "paced"):
    for mode in ("off", "on"):
        r = rows(kind, mode)
        med = lambda k: statistics.median(float(x[k]) for x in r)
        lat = (f"{med('total_p50_ns')/1e3:.2f} us | {med('total_p99_ns')/1e3:.2f} us | {med('total_p999_ns')/1e3:.2f} us"
               if kind == "paced" else "backlog | backlog | backlog")
        print(f"| {kind} | {mode} | {len(r)} | {med('book_msgs_per_sec')/1e6:.2f} M | {lat} |")
subs = [l for p in sorted((res / "logs").glob("sub_*.txt")) for l in p.read_text().splitlines() if l.startswith("received")]
print("\nSubscriber (per invocation):")
for l in subs: print("  " + l)
PY
