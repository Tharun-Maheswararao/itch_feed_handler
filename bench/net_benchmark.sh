#!/usr/bin/env bash
# Version 2 results on one Linux host (Stage C4 table, one-host rows).
#
#   sudo bench/net_benchmark.sh BUILD_DIR ITCH_FILE GOLDEN_HASHES
#
# For each receive path (kernel sockets, AF_XDP), through two network
# namespaces and a veth pair (bench/netns_feed.sh):
#   1. a verification run under seeded loss on both lines, recovery on, every
#      checkpoint checked including queue order ("book match after loss")
#   2. RUNS clean latency runs at RATE, pinned, level digest checked at every
#      checkpoint (the queue-order walk would stall the book thread)
#   3. one full-speed run: as fast as the publisher can send, recovery on
# plus Version 1's file replay at the same rate on the same host, for
# reference. Writes $OUT/*.json, the logs, and $OUT/SUMMARY.md.
#
# Environment: RATE=2000000 RUNS=3 TRANSPORTS="udp xdp" OUT=results/v2/net/<host>
#              RUN_TIMEOUT=1500 (seconds; a run that takes longer is stopped and fails)
set -euo pipefail

BUILD=$1 FILE=$2 GOLDEN=$3
RATE=${RATE:-2000000}
RUNS=${RUNS:-3}
TRANSPORTS=${TRANSPORTS:-"udp xdp"}
OUT=${OUT:-results/v2/net/$(hostname -s)}
mkdir -p "$OUT/logs"

# Distinct physical cores on socket 0: publisher, receiver, book, Version 1.
mapfile -t CORES < <(lscpu -p=CPU,CORE,SOCKET | grep -v '^#' | awk -F, '$3==0 && !seen[$2]++ {print $1}')
if [ "${#CORES[@]}" -lt 4 ]; then
  echo "need 4 physical cores, found ${#CORES[@]}" >&2
  exit 1
fi
PUB=${CORES[1]} RX=${CORES[2]} BOOK=${CORES[3]}
echo "cores: publisher $PUB, receiver $RX, book $BOOK; rate $RATE; runs $RUNS" | tee "$OUT/logs/setup.txt"

LOSS=(--seed 42 --drop-a 0.001 --drop-b 0.001 --drop-both 0.0002 --drop-range 100000-100499:ab --duplicate 0.0005 --reorder 0.0005)
PIN=(--cpu-rx "$RX" --cpu-book "$BOOK")

net_run() {  # name transport rate [receiver args...] --- [publisher args...]
  local name=$1 transport=$2 rate=$3
  shift 3
  echo "== $name"
  # A ceiling per run, so one stuck run cannot use up the whole job.
  OUT="$OUT/logs/$name" TRANSPORT=$transport RATE=$rate PUB_CPU=$PUB \
    timeout -k 30 "${RUN_TIMEOUT:-1500}" bench/netns_feed.sh "$BUILD" "$FILE" "$GOLDEN" -- --json "$OUT/$name.json" --start-timeout-ms 180000 "$@" \
    > "$OUT/logs/$name.txt" 2>&1 || echo "   FAILED (see $OUT/logs/$name.txt)"
  grep -E "^receiver:|wire to book|gaps [0-9]|^PASS|^FAIL" "$OUT/logs/$name/receiver.txt" | sed 's/^/   /' || true
}

for t in $TRANSPORTS; do
  net_run "${t}_loss" "$t" "$RATE" "${PIN[@]}" --- "${LOSS[@]}"
  for i in $(seq 1 "$RUNS"); do
    net_run "${t}_run$i" "$t" "$RATE" "${PIN[@]}" --no-queue-hash
  done
  net_run "${t}_fullspeed" "$t" 0 "${PIN[@]}" --no-queue-hash
done

echo "== Version 1 file replay (reference)"
"$BUILD/feed_handler" bench "$FILE" --rate "$RATE" --runs "$RUNS" --warmup 1 \
  --cpu-producer "$RX" --cpu-consumer "$BOOK" --out "$OUT/v1" > "$OUT/logs/v1.txt" 2>&1
tail -3 "$OUT/logs/v1.txt"

python3 scripts/net_table.py --results "$OUT" --rate "$RATE" > "$OUT/SUMMARY.md"
cat "$OUT/SUMMARY.md"
