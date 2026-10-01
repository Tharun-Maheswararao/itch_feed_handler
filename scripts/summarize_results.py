#!/usr/bin/env python3
"""Markdown summary of a benchmark results directory.

    python3 scripts/summarize_results.py --results results/linux > results/linux/SUMMARY.md

Reads <results>/<config>/run_stats.csv (written by `feed_handler bench`) and
the logs written by bench/run_benchmark.sh. Used as the pull-request body by
.github/workflows/benchmark.yml.
"""
import argparse
from pathlib import Path

import pandas as pd


def fmt_ns(v):
    v = float(v)
    if v < 1e3:
        return f"{v:.0f} ns"
    if v < 1e6:
        return f"{v / 1e3:.3g} µs"
    if v < 1e9:
        return f"{v / 1e6:.3g} ms"
    return f"{v / 1e9:.3g} s"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", type=Path, default=Path("results"))
    ap.add_argument("--title", default="Benchmark results")
    ap.add_argument("--run-url", default="")
    ap.add_argument("--docs", default="docs/linux", help="where the charts were written")
    args = ap.parse_args()

    configs = sorted(p for p in args.results.iterdir() if (p / "run_stats.csv").exists())
    if not configs:
        raise SystemExit(f"no */run_stats.csv under {args.results}")
    order = {"unpaced": 0}
    configs.sort(key=lambda p: (order.get(p.name, 1), p.name))
    stats = {p.name: pd.read_csv(p / "run_stats.csv") for p in configs}

    first = next(iter(stats.values())).iloc[0]
    pinned = all(bool(d["producer_pinned"].all() and d["consumer_pinned"].all()) for d in stats.values())
    out = [f"## {args.title}", ""]
    if args.run_url:
        out += [f"Workflow run: {args.run_url}", ""]
    out += [
        "| Host | |",
        "|---|---|",
        f"| CPU | {first['cpu_model']} ({first['physical_cores']} physical / {first['logical_cores']} logical cores) |",
        f"| OS | {first['os']} |",
        f"| Compiler | {first['compiler']} |",
        f"| Flags | `{first['flags']}` |",
        f"| Clock | {first['clock_source']}, {float(first['clock_ns_per_tick']):.2f} ns/tick, smallest step between two reads "
        f"{float(first.get('clock_resolution_ns', first['clock_ns_per_tick'])):.1f} ns |",
        f"| Threads pinned | {'yes' if pinned else '**no**'} |",
        "",
        "**End-to-end latency** (parser stamp → book updated), median of the runs:",
        "",
        "| Configuration | Book msgs | Throughput (median, range) | p50 | p90 | p99 | p99.9 | max | book p50 | book p99 |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for name, d in stats.items():
        m = d[d["median"] == 1].iloc[0]
        tput = f"{m.book_msgs_per_sec / 1e6:.2f} M/s ({d.book_msgs_per_sec.min() / 1e6:.2f}–{d.book_msgs_per_sec.max() / 1e6:.2f})"
        cells = [fmt_ns(m[f"total_{k}_ns"]) for k in ("p50", "p90", "p99", "p999", "max")]
        out.append(f"| {name} | {int(m.book_messages):,} | {tput} | " + " | ".join(cells) +
                   f" | {fmt_ns(m.book_p50_ns)} | {fmt_ns(m.book_p99_ns)} |")
    out.append("")
    out.append("Run-to-run spread of end-to-end p99: " + "; ".join(
        f"{n} {fmt_ns(d.total_p99_ns.min())}–{fmt_ns(d.total_p99_ns.max())}" for n, d in stats.items()))
    out.append("")

    logs = args.results / "logs"
    verify = logs / "verify_full_day.txt"
    if verify.exists():
        last = [l for l in verify.read_text().splitlines() if l.startswith(("PASS", "FAIL"))]
        out += [f"**Golden-model check (full day):** {last[-1] if last else 'no verdict found'}", ""]
    micro = logs / "micro_bench.txt"
    if micro.exists():
        out += ["<details><summary>Component micro-benchmarks</summary>", "", "```",
                micro.read_text().strip(), "```", "", "</details>", ""]
    out.append(f"Charts: `{args.docs}/*.png`. Raw data: `{args.results}/*/run_stats.csv` and `histogram.csv`.")
    print("\n".join(out))


if __name__ == "__main__":
    main()
