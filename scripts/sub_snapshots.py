#!/usr/bin/env python3
"""Subscribe to `feed_handler bench --publish` and print the live book.

    python3 scripts/sub_snapshots.py                      # AAPL on tcp://localhost:5556
    python3 scripts/sub_snapshots.py MSFT --endpoint tcp://10.0.0.5:5556 --count 50

Needs pyzmq and protobuf (pip install pyzmq protobuf) and protoc on PATH.

The schema is loaded at run time from proto/book_snapshot.proto (compiled to
a descriptor set with protoc), not from generated *_pb2.py code. Generated
Python code only runs with a matching protobuf runtime version; a descriptor
set works with any 3.20+ runtime, so the script does not care which protoc
or pip protobuf is installed.

Per snapshot: top of book, the publisher's sequence number (a jump means
snapshots were missed, e.g. while connecting or when this script fell
behind) and the send-to-receive latency (same machine, wall clock).
"""
import argparse
import shutil
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path

import zmq
from google.protobuf import descriptor_pb2, message_factory

ROOT = Path(__file__).resolve().parent.parent
PROTO = ROOT / "proto" / "book_snapshot.proto"
CACHE = ROOT / "scripts" / ".gen" / "book_snapshot.desc"


def load_schema():
    if not CACHE.exists() or CACHE.stat().st_mtime < PROTO.stat().st_mtime:
        protoc = shutil.which("protoc")
        if not protoc:
            sys.exit("protoc not found: install protobuf (brew install protobuf / apt install protobuf-compiler)")
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([protoc, f"-I{PROTO.parent}", f"--descriptor_set_out={CACHE}", PROTO.name], check=True)
    fds = descriptor_pb2.FileDescriptorSet.FromString(CACHE.read_bytes())
    return message_factory.GetMessages(fds.file)["fh.wire.BookSnapshot"]


def fmt_time(ns):
    s = ns // 1_000_000_000
    return f"{s // 3600:02d}:{s // 60 % 60:02d}:{s % 60:02d}.{ns // 1_000_000 % 1000:03d}"


def level(levels):
    if not levels:
        return "        -          "
    l = levels[0]
    return f"{l.price / 10000:>10.4f} x {l.shares:<7d}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("symbol", nargs="?", default="AAPL")
    ap.add_argument("--endpoint", default="tcp://localhost:5556")
    ap.add_argument("--count", type=int, default=0, help="exit after N snapshots (0 = run until Ctrl-C)")
    ap.add_argument("--timeout", type=float, default=0, help="exit after N seconds with nothing received")
    ap.add_argument("--quiet", action="store_true", help="print only the summary")
    args = ap.parse_args()
    # SIGTERM (kill, pkill) ends like Ctrl-C, so the summary still prints.
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt))

    Snapshot = load_schema()
    ctx = zmq.Context()
    sock = ctx.socket(zmq.SUB)
    sock.setsockopt(zmq.LINGER, 0)
    if args.timeout:
        sock.setsockopt(zmq.RCVTIMEO, int(args.timeout * 1000))
    sock.setsockopt_string(zmq.SUBSCRIBE, args.symbol)
    sock.connect(args.endpoint)
    print(f"subscribed to {args.symbol} on {args.endpoint}", file=sys.stderr)

    received = missed = 0
    last_seq = None
    lat_us, sizes = [], []
    try:
        while not args.count or received < args.count:
            try:
                topic, payload = sock.recv_multipart()
            except zmq.Again:
                break
            now = time.time_ns()
            if topic.decode() != args.symbol:  # SUBSCRIBE matches prefixes: AAPL would also get AAPLW
                continue
            snap = Snapshot.FromString(payload)
            received += 1
            if last_seq is not None and snap.publish_seq > last_seq + 1:
                missed += snap.publish_seq - last_seq - 1
            last_seq = snap.publish_seq
            lat_us.append((now - snap.send_time_ns) / 1000)
            sizes.append(len(payload))
            if not args.quiet:
                print(f"#{snap.publish_seq:<6d} {snap.symbol:<6s} {fmt_time(snap.itch_time_ns)}  "
                      f"bid {level(snap.bids)} | ask {level(snap.asks)}  "
                      f"book msgs {snap.book_messages:>11,d}  {lat_us[-1]:7.0f} us", flush=True)
    except KeyboardInterrupt:
        pass
    if received:
        lat = sorted(lat_us)
        p99 = lat[min(len(lat) - 1, int(0.99 * len(lat)))]
        print(f"received {received} snapshots, missed {missed} (sequence gaps after the first), "
              f"{statistics.mean(sizes):.0f} bytes mean; send->receive median {statistics.median(lat):.0f} us, "
              f"p99 {p99:.0f} us")
    else:
        print("received nothing")
    return 0 if received else 1


if __name__ == "__main__":
    sys.exit(main())
