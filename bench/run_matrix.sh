#!/usr/bin/env bash
# Stage B benchmark matrix: every queue x capacity x thread placement x load.
#
#   bench/run_matrix.sh data/12302019.NASDAQ_ITCH50
#
# Cells (each is one `feed_handler bench`, median of RUNS after 1 warmup):
#   queue      spsc, spsc_unaligned (false-sharing layout), rigtorp, boost, mutex
#   capacity   1024, 65536, 1048576 slots
#   placement  same-core (two hyperthreads of one physical core),
#              cross-core (two physical cores, same socket),
#              cross-socket (one core on each socket; only on 2-socket hosts)
#   load       unpaced     full-speed replay of 04:00-10:00 (38 M book msgs)
#              paced_itch  follows the ITCH timestamps at SPEEDUP x real time
#                          over 09:30:00-09:33:00 (the open); messages before
#                          09:30 only build the book and are not timed
# Same message size, compiler flags, pinning and 1-in-SAMPLE latency sampling
# for every queue.
#
# Bash 3.2 compatible (macOS). Environment overrides: QUEUES, CAPS, PLACEMENTS, LOADS, RUNS (3), SAMPLE (16),
# SPEEDUP (10), OUT (results/matrix/<host>), DOCS (docs/matrix/<host>).
set -euo pipefail

DATA=${1:?usage: bench/run_matrix.sh ITCH_FILE}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build-bench
RUNS=${RUNS:-3}
SAMPLE=${SAMPLE:-16}
SPEEDUP=${SPEEDUP:-10}
CAPS=${CAPS:-"1024 65536 1048576"}
LOADS=${LOADS:-"unpaced paced_itch"}

echo "== build (Release, native)"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DFH_NATIVE=ON -DFH_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN)" --target feed_handler >/dev/null
FH=$BUILD/feed_handler

# Available queues come from the binary (boost only if it was found).
QUEUES=${QUEUES:-$("$FH" queues | tr '\n' ' ')}

# Host name for output directories: EC2 instance type, else the CPU model.
HOST=$(curl -s -m 2 http://169.254.169.254/latest/meta-data/instance-type 2>/dev/null || true)
if [[ -z "$HOST" ]]; then
  TOKEN=$(curl -s -m 2 -X PUT http://169.254.169.254/latest/api/token -H "X-aws-ec2-metadata-token-ttl-seconds: 60" 2>/dev/null || true)
  [[ -n "$TOKEN" ]] && HOST=$(curl -s -m 2 -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/instance-type || true)
fi
[[ -z "$HOST" ]] && HOST=$(uname -s | tr '[:upper:]' '[:lower:]')
OUT=${OUT:-$ROOT/results/matrix/$HOST}
DOCS=${DOCS:-$ROOT/docs/matrix/$HOST}
mkdir -p "$OUT/logs"

# Placements -> "producer_cpu consumer_cpu", kept in PIN_<name> variables
# (plain variables: macOS ships bash 3.2, which has no associative arrays).
pin_var() { echo "PIN_${1//-/_}"; }
DEFAULT_PLACEMENTS=""
if command -v lscpu >/dev/null; then
  lscpu > "$OUT/logs/lscpu.txt"
  topo=$(lscpu -p=CPU,CORE,SOCKET | grep -v '^#')
  # first CPU of each physical core, per socket, skipping core 0
  mapfile -t s0 < <(awk -F, '$3==0 && !seen[$2]++ {print $1}' <<<"$topo")
  mapfile -t s1 < <(awk -F, '$3==1 && !seen[$2]++ {print $1}' <<<"$topo")
  p=${s0[1]}
  core_p=$(awk -F, -v p="$p" '$1==p {print $2}' <<<"$topo")
  sib=$(awk -F, -v c="$core_p" -v p="$p" '$2==c && $1!=p {print $1; exit}' <<<"$topo")
  if [[ -n "$sib" ]]; then PIN_same_core="$p $sib"; DEFAULT_PLACEMENTS+="same-core "; fi
  PIN_cross_core="$p ${s0[2]}"; DEFAULT_PLACEMENTS+="cross-core "
  if (( ${#s1[@]} > 1 )); then PIN_cross_socket="$p ${s1[1]}"; DEFAULT_PLACEMENTS+="cross-socket "; fi
else
  PIN_unpinned="-1 -1"
  DEFAULT_PLACEMENTS="unpinned"
fi
PLACEMENTS=${PLACEMENTS:-$DEFAULT_PLACEMENTS}
echo "== host $HOST | queues: $QUEUES | caps: $CAPS | placements: $PLACEMENTS | loads: $LOADS | runs $RUNS"
for k in $PLACEMENTS; do v=$(pin_var "$k"); echo "   $k -> CPUs ${!v:?no CPUs for placement $k on this host}"; done

cells=0
for load in $LOADS; do
  case $load in
    unpaced)    LOAD_ARGS=(--until 10:00:00) ;;
    paced_itch) LOAD_ARGS=(--speedup "$SPEEDUP" --pace-from 09:30:00 --until 09:33:00) ;;
    *) echo "unknown load $load" >&2; exit 1 ;;
  esac
  for place in $PLACEMENTS; do
    v=$(pin_var "$place"); read -r P C <<<"${!v}"
    for cap in $CAPS; do
      for q in $QUEUES; do
        label="${q}_${cap}_${place}_${load}"
        printf '== %-48s ' "$label"
        "$FH" bench "$DATA" --book fast --queue "$q" --capacity "$cap" --placement "$place" \
          --cpu-producer "$P" --cpu-consumer "$C" --sample-every "$SAMPLE" \
          --runs "$RUNS" --warmup 1 --label "$label" --out "$OUT/$label" "${LOAD_ARGS[@]}" \
          > "$OUT/logs/$label.txt" 2>&1
        grep -E "^Run" "$OUT/logs/$label.txt" | tail -1 | grep -oE "[0-9.]+ M book msgs/s" || echo "(see log)"
        cells=$((cells + 1))
      done
    done
  done
done
echo "== $cells cells done"

PY=python3
for c in ${PYTHON:+"$PYTHON"} python3 /Library/Frameworks/Python.framework/Versions/Current/bin/python3 /usr/bin/python3; do
  if "$c" -c "import pandas, matplotlib" >/dev/null 2>&1; then PY=$c; break; fi
done
"$PY" "$ROOT/scripts/matrix_table.py" --results "$OUT" > "$OUT/TABLE.md"
"$PY" "$ROOT/scripts/plot_matrix.py" --results "$OUT" --out "$DOCS"
echo "== wrote $OUT/TABLE.md and charts in $DOCS"
