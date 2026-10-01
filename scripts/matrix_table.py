#!/usr/bin/env python3
"""One Markdown table with every cell of the Stage B benchmark matrix.

    python3 scripts/matrix_table.py --results results/matrix/c7i.2xlarge > TABLE.md

Reads <results>/<cell>/run_stats.csv written by bench/run_matrix.sh and
prints the median run of each cell: throughput and end-to-end percentiles.
"""
import argparse
from pathlib import Path

import pandas as pd

QUEUE_ORDER = ["spsc", "spsc_unaligned", "rigtorp", "boost", "mutex"]
PLACE_ORDER = ["same-core", "cross-core", "cross-socket", "unpinned"]
LOAD_ORDER = ["unpaced", "paced_itch"]


def fmt_ns(v):
    v = float(v)
    if v <= 0:
        return "–"
    if v < 1e3:
        return f"{v:.0f} ns"
    if v < 1e6:
        return f"{v / 1e3:.3g} µs"
    if v < 1e9:
        return f"{v / 1e6:.3g} ms"
    return f"{v / 1e9:.3g} s"


def fmt_cap(c):
    c = int(c)
    return f"{c // 1048576}M" if c >= 1048576 else (f"{c // 1024}K" if c >= 1024 else str(c))


def load_matrix(root: Path) -> pd.DataFrame:
    frames = []
    for f in sorted(root.glob("*/run_stats.csv")):
        d = pd.read_csv(f)
        d["cell"] = f.parent.name
        d["load"] = d["paced"].map({1: "paced_itch", 0: "unpaced", True: "paced_itch", False: "unpaced"})
        frames.append(d)
    if not frames:
        raise SystemExit(f"no */run_stats.csv under {root}")
    return pd.concat(frames, ignore_index=True)


def medians(df: pd.DataFrame) -> pd.DataFrame:
    m = df[df["median"] == 1].copy()
    key = lambda col, order: m[col].map(lambda v: order.index(v) if v in order else len(order))
    m["_q"], m["_p"], m["_l"] = key("queue", QUEUE_ORDER), key("placement", PLACE_ORDER), key("load", LOAD_ORDER)
    return m.sort_values(["_l", "_p", "capacity", "_q"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", type=Path, required=True)
    args = ap.parse_args()
    df = load_matrix(args.results)
    m = medians(df)
    first = m.iloc[0]
    runs = df.groupby("cell")["run"].count().max()
    print(f"## Queue benchmark matrix: {first['cpu_model']}")
    print()
    print(f"{len(m)} cells, median of {runs} runs each after one warmup. {first['os']}, {first['compiler']}, "
          f"`{first['flags']}`. Latency sampled 1 in {int(first['sample_every'])}. "
          "unpaced = full-speed replay of 04:00–10:00; paced_itch = ITCH timestamps at 10x over 09:30–09:33.")
    versions = m.drop_duplicates("queue").set_index("queue")["queue_version"].to_dict()
    print()
    print("Queues: " + "; ".join(f"`{q}` = {versions[q]}" for q in QUEUE_ORDER if q in versions) + ".")
    print()
    print("| Load | Placement | Capacity | Queue | Throughput | p50 | p90 | p99 | p99.9 | max |")
    print("|---|---|---:|---|---:|---:|---:|---:|---:|---:|")
    for _, r in m.iterrows():
        tput = f"{r.book_msgs_per_sec / 1e6:.2f} M/s"
        print(f"| {r.load} | {r.placement} | {fmt_cap(r.capacity)} | {r.queue} | {tput} | "
              + " | ".join(fmt_ns(r[f'total_{k}_ns']) for k in ("p50", "p90", "p99", "p999", "max")) + " |")


if __name__ == "__main__":
    main()
