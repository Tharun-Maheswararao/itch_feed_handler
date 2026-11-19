#!/usr/bin/env python3
"""Build the Version 2 results table (ROADMAP_V2.md section 9) from the
receiver's JSON files and Version 1's run_stats.csv written by
bench/net_benchmark.sh (one host) or bench/net_two_host.sh (two hosts).

  python3 scripts/net_table.py --results results/v2/net/<host> [--rate R] [--title T]

Medians over the clean runs, like every other table in this repo; the loss
run supplies the "book match after loss" column and the recovery numbers.
"""
import argparse
import csv
import glob
import json
import os
import statistics
import sys

PATHS = [
    ("udp", "Kernel sockets ({where})"),
    ("xdp", "AF_XDP ({where})"),
    ("dpdk", "DPDK ({where})"),
]


def us(ns):
    return f"{ns / 1000:.2f} µs" if ns < 1_000_000 else f"{ns / 1e6:.2f} ms"


def load(path):
    with open(path) as f:
        return json.loads(f.read())


def publisher_rate(path):
    """msgs/s from the publisher's "published N messages in P packets, S s, R msgs/s" line."""
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("published "):
                    return float(line.rsplit(",", 1)[1].split()[0])
    except (OSError, ValueError, IndexError):
        pass
    return 0.0


def med(rows, key):
    return statistics.median(r[key] for r in rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--rate", type=float, default=0)
    ap.add_argument("--where", default="one host, veth, multicast")
    ap.add_argument("--title", default="")
    a = ap.parse_args()
    R = a.results

    out = []
    title = a.title or f"Version 2 results: {os.path.basename(os.path.normpath(R))}"
    out.append(f"# {title}\n")
    if a.rate:
        out.append(f"Paced at {a.rate / 1e6:g} M msgs/s; latency is the median of the clean runs' "
                   "percentiles; every run is checked against the golden model.\n")
    out.append("| Path | Timestamp source | p50 | p99 | p99.9 | Max | Messages/s | Book match after loss |")
    out.append("|---|---|---:|---:|---:|---:|---:|---|")

    v1 = os.path.join(R, "v1", "run_stats.csv")
    if os.path.exists(v1):
        with open(v1) as f:
            rows = [r for r in csv.DictReader(f)]
        if rows:
            def m(k):
                return statistics.median(float(r[k]) for r in rows)
            out.append(f"| File replay (Version 1, parse to book) | cycle counter ({rows[0].get('clock_source', 'rdtsc')}) | "
                       f"{us(m('total_p50_ns'))} | {us(m('total_p99_ns'))} | {us(m('total_p999_ns'))} | "
                       f"{us(m('total_max_ns'))} | {m('file_msgs_per_sec') / 1e6:.2f} M | reference |")

    details = []
    for key, label in PATHS:
        runs = [load(p) for p in sorted(glob.glob(os.path.join(R, f"{key}_run*.json")))]
        loss_p = os.path.join(R, f"{key}_loss.json")
        loss = load(loss_p) if os.path.exists(loss_p) else None
        if not runs and not loss:
            continue
        name = label.format(where=a.where)
        if runs:
            ok_runs = all(r["passed"] for r in runs)
            src = runs[0]["timestamp_source"].replace(" timestamp", "")
            cells = [us(med(runs, k)) for k in ("p50_ns", "p99_ns", "p999_ns", "max_ns")]
            rate = f"{med(runs, 'msgs_per_sec') / 1e6:.2f} M"
            transport = runs[0]["transport"]
        else:
            ok_runs, src, cells, rate, transport = True, loss["timestamp_source"], ["-"] * 4, "-", loss["transport"]
        if loss:
            if loss["passed"]:
                match = (f"PASS: {loss['gaps']} gaps ({loss['gaps_by_line']} other line, "
                         f"{loss['gaps_by_retransmit']} re-request, {loss['gaps_by_snapshot']} snapshot)")
            else:
                match = "FAIL"
        else:
            match = "not run"
        if not ok_runs:
            match += "; a clean run FAILED"
        out.append(f"| {name} | {src} | {' | '.join(cells)} | {rate} | {match} |")
        details.append((key, transport, runs, loss))

    for key, transport, runs, loss in details:
        out.append(f"\n## {transport}\n")
        out.append("| Run | Passed | p50 | p99 | p99.9 | Max | Messages/s | Gaps | Kernel/NIC drops |")
        out.append("|---|---|---:|---:|---:|---:|---:|---:|---:|")
        for i, r in enumerate(runs, 1):
            out.append(f"| clean {i} | {'yes' if r['passed'] else 'NO'} | {us(r['p50_ns'])} | {us(r['p99_ns'])} | "
                       f"{us(r['p999_ns'])} | {us(r['max_ns'])} | {r['msgs_per_sec'] / 1e6:.2f} M | {r['gaps']} | "
                       f"{r['kernel_drops']} |")
        if loss:
            out.append(f"| seeded loss | {'yes' if loss['passed'] else 'NO'} | {us(loss['p50_ns'])} | "
                       f"{us(loss['p99_ns'])} | {us(loss['p999_ns'])} | {us(loss['max_ns'])} | "
                       f"{loss['msgs_per_sec'] / 1e6:.2f} M | {loss['gaps']} | {loss['kernel_drops']} |")
            if loss["gaps_by_retransmit"]:
                out.append(f"\nRe-request recovery time: p50 {us(loss['recovery_p50_ns'])}, "
                           f"p99 {us(loss['recovery_p99_ns'])}; {loss['packets_recovered']} packets recovered. "
                           f"Checkpoints: {loss['checkpoints_matched']} of {loss['checkpoints']} matched "
                           f"(level digest and queue order), final book "
                           f"{'matches' if loss['final_match'] else 'DIFFERS'}. Heap allocations after warmup: "
                           f"{loss['allocations_after_warmup']}.")
        fs_p = os.path.join(R, f"{key}_fullspeed.json")
        if os.path.exists(fs_p):
            fs = load(fs_p)
            sent = publisher_rate(os.path.join(R, "logs", f"{key}_fullspeed", "publisher.txt"))
            sent_txt = f"the publisher sent {sent / 1e6:.2f} M msgs/s" if sent else "the publisher sent as fast as it could"
            how = (f"{fs['gaps']} gaps: {fs['gaps_by_line']} filled by the other line, "
                   f"{fs['gaps_by_retransmit']} by re-request ({fs['packets_recovered']} packets), "
                   f"{fs.get('snapshots_loaded', fs['gaps_by_snapshot'])} snapshot loads")
            if fs["passed"]:
                verdict = "book verified at every checkpoint after the last snapshot and at the end"
            else:
                verdict = f"book NOT verified (stopped: {fs['stop_reason']})"
            out.append(f"\nFull speed: {sent_txt}; {fs['kernel_drops']} kernel/NIC drops; {how}; {verdict}. "
                       "This is a stress test of recovery, not a throughput figure: what the receiver "
                       "could not take directly came from re-requests and snapshots.")

    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
