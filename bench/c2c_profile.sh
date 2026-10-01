#!/usr/bin/env bash
# Cache-to-cache contention profile of the ring, with `perf c2c`.
#
#   bench/c2c_profile.sh data/12302019.NASDAQ_ITCH50 OUT_DIR [queue ...]
#
# Runs the same unpaced, cross-core, 64K-slot pipeline for each queue
# (default: spsc rigtorp) under `perf c2c record`, and keeps the text reports:
#   c2c_<queue>_stats.txt   totals: loads, HITM (loads that hit a line modified
#                           in another core's cache), store/load counts
#   c2c_<queue>_lines.txt   the most contended cache lines and the code
#                           touching them
#   c2c_<queue>_stat.txt    perf stat counters for the same run
# Needs a full PMU (PEBS load latency): bare metal, not most VMs. Best effort:
# exits 0 with a note if perf c2c is unavailable.
set -uo pipefail

DATA=${1:?usage: bench/c2c_profile.sh ITCH_FILE OUT_DIR [queue ...]}
OUT=${2:?usage: bench/c2c_profile.sh ITCH_FILE OUT_DIR [queue ...]}
shift 2
QUEUES=${*:-spsc rigtorp}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FH=$ROOT/build-bench/feed_handler
mkdir -p "$OUT"

# The `perf` wrapper refuses to run if its version does not match the kernel;
# fall back to any installed linux-tools perf binary.
PERF=$(command -v perf || true)
if [[ -z "$PERF" ]] || ! "$PERF" --version >/dev/null 2>&1; then
  PERF=$(ls /usr/lib/linux-tools/*/perf 2>/dev/null | tail -1 || true)
fi
if [[ -z "$PERF" ]] || ! "$PERF" c2c record -o /tmp/c2c_probe.data -- true >/dev/null 2>&1; then
  echo "perf c2c not available on this host; skipping" | tee "$OUT/c2c_unavailable.txt"
  exit 0
fi
echo "== using $PERF ($("$PERF" --version))"

cores=($(lscpu -p=CPU,CORE,SOCKET | grep -v '^#' | awk -F, '$3==0 && !seen[$2]++ {print $1}'))
P=${cores[1]}; C=${cores[2]}
for q in $QUEUES; do
  echo "== perf c2c: $q (producer CPU $P, consumer CPU $C)"
  args=(bench "$DATA" --book fast --queue "$q" --capacity 65536 --until 10:00:00 --runs 1 --warmup 0
        --cpu-producer "$P" --cpu-consumer "$C" --sample-every 16 --label "c2c_$q" --out "$OUT/run_$q")
  "$PERF" c2c record -o "$OUT/c2c_$q.data" -- "$FH" "${args[@]}" > "$OUT/c2c_${q}_run.txt" 2>&1
  "$PERF" c2c report -i "$OUT/c2c_$q.data" --stdio --stats > "$OUT/c2c_${q}_stats.txt" 2>&1
  "$PERF" c2c report -i "$OUT/c2c_$q.data" --stdio --full-symbols -c tid,iaddr 2>/dev/null \
    | head -400 > "$OUT/c2c_${q}_lines.txt"
  "$PERF" stat -e cycles,instructions,L1-dcache-load-misses,LLC-load-misses,LLC-store-misses \
    -o "$OUT/c2c_${q}_stat.txt" -- "$FH" "${args[@]}" >/dev/null 2>&1
  rm -f "$OUT/c2c_$q.data"  # raw samples are large; keep the text reports
  grep -E "HITM|Load Local|Load Remote|Total records" "$OUT/c2c_${q}_stats.txt" | head -12
done
echo "== c2c reports in $OUT"
