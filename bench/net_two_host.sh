#!/usr/bin/env bash
# Version 2 between two hosts: the Stage C2 two-host check and the Stage C4
# two-host rows. A VPC carries no IP multicast without a Transit Gateway, so
# the publisher sends unicast UDP to two ports on the receiver, one per line;
# arbitration only looks at sequence numbers, so nothing else changes.
#
#   receiver host:   sudo bench/net_two_host.sh receive BUILD_DIR GOLDEN PUBLISHER_IP
#   publisher host:  bench/net_two_host.sh publish BUILD_DIR ITCH_FILE RECEIVER_IP
#
# Both sides walk the same list of runs. Before each run the receiver opens
# TCP port 31900 once it is listening; the publisher waits for that, then
# sends. Ports (allow them between the two hosts): UDP 31001-31003 (lines A
# and B, retransmit requests), TCP 31004 (snapshots), TCP 31900 (ready).
#
# Environment (must match on both sides): RATE=2000000 RUNS=3 TRANSPORTS="udp xdp"
# Receiver only: OUT=results/v2/net2/<host>. AF_XDP needs a build with
# -DFH_ENABLE_XDP=ON; it is skipped if the NIC cannot be set to one queue.
set -euo pipefail

ROLE=$1 BUILD=$2
RATE=${RATE:-2000000}
RUNS=${RUNS:-3}
TRANSPORTS=${TRANSPORTS:-"udp xdp"}
READY_PORT=31900

mapfile -t CORES < <(lscpu -p=CPU,CORE,SOCKET | grep -v '^#' | awk -F, '$3==0 && !seen[$2]++ {print $1}')
LOSS=(--seed 42 --drop-a 0.001 --drop-b 0.001 --drop-both 0.0002 --drop-range 100000-100499:ab --duplicate 0.0005 --reorder 0.0005)

# name transport rate loss(0/1) queue_hash(0/1), identical on both sides
PLAN=()
for t in $TRANSPORTS; do
  PLAN+=("${t}_loss $t $RATE 1 1")
  for i in $(seq 1 "$RUNS"); do PLAN+=("${t}_run$i $t $RATE 0 0"); done
  PLAN+=("${t}_fullspeed $t 0 0 0")
done

if [ "$ROLE" = publish ]; then
  FILE=$3 RX_IP=$4
  # The receiver opens the ready port when it is listening for the next run,
  # which also means it has finished the previous one: only then is the
  # previous publisher (still serving retransmits and snapshots) stopped.
  # After the last run the receiver opens the port once more to say "done".
  prev=""
  stop_prev() {
    if [ -n "$prev" ]; then
      kill -TERM "$prev" 2>/dev/null || true
      wait "$prev" 2>/dev/null || true
      prev=""
    fi
  }
  trap stop_prev EXIT
  wait_ready() {
    local deadline=$((SECONDS + 3600))
    until python3 -c "import socket; socket.create_connection(('$RX_IP', $READY_PORT), 2).close()" 2>/dev/null; do
      if [ "$SECONDS" -gt "$deadline" ]; then
        echo "receiver $RX_IP never opened TCP $READY_PORT: check the security group (docs/AWS_BENCHMARK.md)" >&2
        exit 1
      fi
      sleep 1
    done
  }
  for entry in "${PLAN[@]}"; do
    read -r name transport rate loss _ <<< "$entry"
    echo "== $name: waiting for the receiver"
    wait_ready
    stop_prev
    args=()
    [ "$loss" = 1 ] && args=("${LOSS[@]}")
    taskset -c "${CORES[1]}" "$BUILD/itch_publisher" "$FILE" --line-a "$RX_IP:31001" --line-b "$RX_IP:31002" \
      --rate "$rate" --retransmit-listen 0.0.0.0:31003 --snapshot-listen 0.0.0.0:31004 --linger-ms 900000 \
      ${args[@]+"${args[@]}"} > "publisher_$name.txt" 2>&1 &
    prev=$!
  done
  echo "== waiting for the receiver to finish"
  wait_ready
  stop_prev
  for entry in "${PLAN[@]}"; do
    read -r name _ <<< "$entry"
    echo "== $name"; grep -E "^published|line [AB]:|retransmit:|snapshots:" "publisher_$name.txt" | sed 's/^/   /'
  done
  exit 0
fi

[ "$ROLE" = receive ] || { echo "role must be receive or publish" >&2; exit 2; }
GOLDEN=$3 PUB_IP=$4
OUT=${OUT:-results/v2/net2/$(hostname -s)}
mkdir -p "$OUT/logs"
IFACE=$(ip -o route get "$PUB_IP" | awk '{for (i = 1; i <= NF; i++) if ($i == "dev") print $(i + 1)}')
RX=${CORES[2]} BOOK=${CORES[3]}
sysctl -qw net.core.rmem_max=67108864 2>/dev/null || true
{ echo "interface $IFACE, receiver core $RX, book core $BOOK, rate $RATE"
  ethtool -i "$IFACE" 2>/dev/null || true
  echo "hardware timestamping:"; ethtool -T "$IFACE" 2>/dev/null || true
  ethtool -l "$IFACE" 2>/dev/null || true; } > "$OUT/logs/setup.txt"

# AF_XDP binds one receive queue, so every feed packet must land on it: one
# channel. ENA resets the device for this, so it is done once, and undone.
XDP_OK=0 CHANNELS=""
if [[ " $TRANSPORTS " == *" xdp "* ]]; then
  # `|| true`: with pipefail a missing ethtool, or a NIC that has no channel
  # settings, would otherwise end the script here, and the publisher would
  # wait for a receiver that never comes.
  CHANNELS=$(ethtool -l "$IFACE" 2>/dev/null | awk '/Current hardware settings/ {f = 1} f && /Combined/ {print $2; exit}') || true
  if [ -n "$CHANNELS" ] && ethtool -L "$IFACE" combined 1 2>>"$OUT/logs/setup.txt"; then
    sleep 5
    XDP_OK=1
  else
    echo "AF_XDP skipped: cannot set $IFACE to one channel" | tee -a "$OUT/logs/setup.txt"
  fi
fi
restore() { [ "$XDP_OK" = 1 ] && [ -n "$CHANNELS" ] && ethtool -L "$IFACE" combined "$CHANNELS" || true; }
trap restore EXIT

for entry in "${PLAN[@]}"; do
  read -r name transport rate loss qhash <<< "$entry"
  echo "== $name"
  lines=(--line-a 0.0.0.0:31001 --line-b 0.0.0.0:31002 --rcvbuf 33554432)
  if [ "$transport" = xdp ]; then
    if [ "$XDP_OK" != 1 ]; then
      # Still walk the plan: the publisher is waiting for this run.
      timeout 1800 python3 -c "import socket; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('0.0.0.0', $READY_PORT)); s.listen(1); s.accept()[0].close()"
      sleep 1
      continue
    fi
    lines=(--transport xdp --xdp-if "$IFACE" --xdp-zero-copy --line-a 0.0.0.0:31001 --line-b 0.0.0.0:31002)
  fi
  extra=()
  [ "$qhash" = 0 ] && extra=(--no-queue-hash)
  timeout -k 30 2400 "$BUILD/itch_receiver" "${lines[@]}" --golden "$GOLDEN" --retransmit "$PUB_IP:31003" --snapshot "$PUB_IP:31004" \
    --cpu-rx "$RX" --cpu-book "$BOOK" --json "$OUT/$name.json" --start-timeout-ms 900000 --idle-timeout-ms 10000 \
    ${extra[@]+"${extra[@]}"} > "$OUT/logs/$name.txt" 2>&1 &
  pid=$!
  until grep -q "receiver ready" "$OUT/logs/$name.txt" 2>/dev/null; do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.2
  done
  timeout 1800 python3 -c "import socket; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('0.0.0.0', $READY_PORT)); s.listen(1); s.accept()[0].close()"
  wait "$pid" || echo "   FAILED"
  grep -E "^receiver:|wire to book|gaps [0-9]|^PASS|^FAIL" "$OUT/logs/$name.txt" | sed 's/^/   /' || true
done

# Tell the publisher the last run is over.
timeout 1800 python3 -c "import socket; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('0.0.0.0', $READY_PORT)); s.listen(1); s.accept()[0].close()" || true

python3 scripts/net_table.py --results "$OUT" --rate "$RATE" --where "two hosts, unicast" \
  --title "Version 2 results: two hosts ($(hostname -s))" > "$OUT/SUMMARY.md"
cat "$OUT/SUMMARY.md"
