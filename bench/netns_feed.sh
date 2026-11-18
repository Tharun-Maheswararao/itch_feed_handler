#!/usr/bin/env bash
# Version 2 on one Linux host: the publisher and the receiver in two network
# namespaces joined by a veth pair, lines A and B on two multicast groups.
# This exercises real group membership (IP_ADD_MEMBERSHIP on the receiver's
# interface), which loopback alone does not.
#
#   sudo bench/netns_feed.sh BUILD_DIR FEED GOLDEN [-- receiver args] [--- publisher args]
#
# The receiver gets the retransmit and snapshot servers' addresses, so loss
# injected through publisher args is recovered. Exit status is the
# receiver's: 0 only if the book matched the golden file at every checkpoint.
#
# Environment:
#   RATE=2000000     publisher messages per second
#   TRANSPORT=udp    udp (kernel sockets), xdp (AF_XDP, -DFH_ENABLE_XDP=ON) or
#                    dpdk (DPDK's af_packet device on the veth, -DFH_ENABLE_DPDK=ON)
#   OUT=netns_out    directory for the two logs
#   PUB_CPU=N        pin the publisher to CPU N (taskset); pin the receiver with --cpu-rx/--cpu-book
#   KEEP=1           leave the namespaces up afterwards
set -euo pipefail

if [ $# -lt 3 ]; then
  echo "usage: sudo $0 BUILD_DIR FEED GOLDEN [-- receiver args] [--- publisher args]" >&2
  exit 2
fi
BUILD=$1 FEED=$2 GOLDEN=$3
shift 3
RX_ARGS=() PUB_ARGS=()
mode=""
for a in "$@"; do
  case "$a" in
    --) mode=rx ;;
    ---) mode=pub ;;
    *) if [ "$mode" = pub ]; then PUB_ARGS+=("$a"); else RX_ARGS+=("$a"); fi ;;
  esac
done
RATE=${RATE:-2000000}
TRANSPORT=${TRANSPORT:-udp}
OUT=${OUT:-netns_out}
mkdir -p "$OUT"

PUB_NS=fhpub RX_NS=fhrx
PUB_IF=vpub RX_IF=vrx
PUB_IP=10.77.0.1 RX_IP=10.77.0.2
GROUP_A=239.1.1.1 GROUP_B=239.1.1.2
PORT_A=31001 PORT_B=31002 PORT_RT=31003 PORT_SNAP=31004

cleanup() {
  [ "${KEEP:-0}" = 1 ] && return
  ip netns del "$PUB_NS" 2>/dev/null || true
  ip netns del "$RX_NS" 2>/dev/null || true
}
trap cleanup EXIT
cleanup

ip netns add "$PUB_NS"
ip netns add "$RX_NS"
ip link add "$PUB_IF" type veth peer name "$RX_IF"
ip link set "$PUB_IF" netns "$PUB_NS"
ip link set "$RX_IF" netns "$RX_NS"
for ns in "$PUB_NS" "$RX_NS"; do ip -n "$ns" link set lo up; done
ip -n "$PUB_NS" addr add "$PUB_IP/24" dev "$PUB_IF"
ip -n "$RX_NS" addr add "$RX_IP/24" dev "$RX_IF"
ip -n "$PUB_NS" link set "$PUB_IF" up
ip -n "$RX_NS" link set "$RX_IF" up
# Multicast leaves through the veth; the receiver accepts it there.
ip -n "$PUB_NS" route add 239.0.0.0/8 dev "$PUB_IF"
ip -n "$RX_NS" route add 239.0.0.0/8 dev "$RX_IF"
# rmem_max is host wide on most kernels (not per namespace): raise it there.
sysctl -qw net.core.rmem_max=67108864 2>/dev/null || true
ip netns exec "$RX_NS" sysctl -qw net.ipv4.conf.all.rp_filter=0 2>/dev/null || true
ip netns exec "$RX_NS" sysctl -qw "net.ipv4.conf.$RX_IF.rp_filter=0" 2>/dev/null || true

if [ "$TRANSPORT" = xdp ]; then
  RX_LINES=(--transport xdp --xdp-if "$RX_IF" --line-a "$GROUP_A:$PORT_A" --line-b "$GROUP_B:$PORT_B")
elif [ "$TRANSPORT" = dpdk ]; then
  # Functional test only: DPDK's af_packet device reads the veth, no hugepages, no NIC.
  RX_LINES=(--transport dpdk --line-a "$GROUP_A:$PORT_A" --line-b "$GROUP_B:$PORT_B"
            --dpdk-eal "--no-huge -m 512 --no-pci --no-shconf -l 0 --vdev=net_af_packet0,iface=$RX_IF")
else
  RX_LINES=(--line-a "$GROUP_A:$PORT_A" --line-b "$GROUP_B:$PORT_B" --interface "$RX_IP" --rcvbuf 33554432)
fi

ip netns exec "$RX_NS" "$BUILD/itch_receiver" "${RX_LINES[@]}" \
  --golden "$GOLDEN" --retransmit "$PUB_IP:$PORT_RT" --snapshot "$PUB_IP:$PORT_SNAP" \
  ${RX_ARGS[@]+"${RX_ARGS[@]}"} > "$OUT/receiver.txt" 2>&1 &
RX_PID=$!
for _ in $(seq 1 300); do
  grep -q "receiver ready" "$OUT/receiver.txt" 2>/dev/null && break
  if ! kill -0 "$RX_PID" 2>/dev/null; then cat "$OUT/receiver.txt"; exit 1; fi
  sleep 0.1
done

PUB_PIN=()
[ -n "${PUB_CPU:-}" ] && PUB_PIN=(taskset -c "$PUB_CPU")
ip netns exec "$PUB_NS" ${PUB_PIN[@]+"${PUB_PIN[@]}"} "$BUILD/itch_publisher" "$FEED" \
  --line-a "$GROUP_A:$PORT_A" --line-b "$GROUP_B:$PORT_B" --interface "$PUB_IP" --rate "$RATE" \
  --retransmit-listen "$PUB_IP:$PORT_RT" --snapshot-listen "$PUB_IP:$PORT_SNAP" --linger-ms 3000 \
  ${PUB_ARGS[@]+"${PUB_ARGS[@]}"} > "$OUT/publisher.txt" 2>&1

set +e
wait "$RX_PID"
status=$?
set -e
cat "$OUT/publisher.txt" "$OUT/receiver.txt"
exit $status
