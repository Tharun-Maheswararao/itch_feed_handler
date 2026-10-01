#!/usr/bin/env python3
"""Rebuild every benchmark chart from the CSV output.

    python3 scripts/plot_results.py [--results results] [--out docs]

Reads every <results>/<config>/run_stats.csv and histogram.csv written by
`feed_handler bench` and writes five PNGs into <out>:

    latency_histogram.png   full latency shape, log-log, per pipeline stage
    latency_percentiles.png p50 / p90 / p99 / p99.9 / max per configuration
    latency_breakdown.png   queue wait vs book update, per configuration
    message_mix.png         share of each ITCH message type
    throughput_runs.png     throughput of each of the five runs
"""
import argparse
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402

# Validated categorical slots (fixed order) and neutral inks.
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"

ITCH_NAMES = {
    "A": "Add", "D": "Delete", "U": "Replace", "E": "Executed", "I": "NOII",
    "X": "Cancel", "F": "Add (MPID)", "P": "Trade", "L": "MP position",
    "C": "Exec w/ price", "Q": "Cross", "Y": "Reg SHO", "H": "Trading action",
    "R": "Directory",
}
PCTS = [("p50", "p50"), ("p90", "p90"), ("p99", "p99"), ("p999", "p99.9"), ("max", "max")]


def style():
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "axes.edgecolor": GRID, "axes.labelcolor": INK_2, "axes.titlecolor": INK,
        "axes.titlesize": 13, "axes.titleweight": "bold", "axes.titlelocation": "left",
        "axes.labelsize": 10, "xtick.color": INK_2, "ytick.color": INK_2,
        "xtick.labelsize": 9, "ytick.labelsize": 9, "axes.grid": True, "grid.color": GRID,
        "grid.linewidth": 0.6, "axes.spines.top": False, "axes.spines.right": False,
        "legend.frameon": False, "legend.fontsize": 9, "font.size": 10,
        "lines.linewidth": 2, "figure.dpi": 110,
    })


def fmt_ns(v, _pos=None):
    if v <= 0:
        return "0"
    if v < 1e3:
        return f"{v:.3g} ns"
    if v < 1e6:
        return f"{v / 1e3:.3g} µs"
    if v < 1e9:
        return f"{v / 1e6:.3g} ms"
    return f"{v / 1e9:.3g} s"


def rebin(m, resolution_ns, per_decade=10):
    """Re-bucket a histogram into plotting bins that are never narrower than
    one clock step: linear in clock steps at the low end, logarithmic above.

    The recorded buckets are finer than the clock on some hosts (the Apple M4
    counter steps in 41.67 ns), which would otherwise leave alternating empty
    buckets. Returns the first bin edge, the count below one step, bin
    centres, and counts normalised to "per 1/per_decade decade" so bins of
    different widths are comparable.
    """
    import numpy as np

    lo = m["bucket_lo_ns"].to_numpy()
    cnt = m["count"].to_numpy()
    first = max(resolution_ns, 1.0)
    below = cnt[lo < first * 0.999].sum()
    keep = lo >= first * 0.999
    ratio = 10 ** (1 / per_decade)
    edges = [first * 0.999]
    top = max(lo.max(), first) * ratio * 1.01
    while edges[-1] < top:
        edges.append(max(edges[-1] * ratio, edges[-1] + first))
    edges = np.array(edges)
    hist, _ = np.histogram(lo[keep], bins=edges, weights=cnt[keep])
    width_decades = np.log10(edges[1:] / edges[:-1]) * per_decade
    centers = np.sqrt(edges[:-1] * edges[1:])
    return first, below, centers, hist / width_decades


def load(results: Path):
    stats, hists = [], []
    for d in sorted(p for p in results.iterdir() if (p / "run_stats.csv").exists()):
        s = pd.read_csv(d / "run_stats.csv")
        h = pd.read_csv(d / "histogram.csv")
        s["config"] = d.name
        h["config"] = d.name
        stats.append(s)
        hists.append(h)
    if not stats:
        sys.exit(f"no run_stats.csv under {results}/*/")
    return pd.concat(stats, ignore_index=True), pd.concat(hists, ignore_index=True)


def config_order(stats):
    # Stable order: as listed by --configs, else alphabetical.
    return list(dict.fromkeys(stats["config"]))


def median_rows(stats):
    return stats[stats["median"] == 1].set_index("config")


def caption(fig, text):
    fig.text(0.01, 0.01, text, ha="left", va="bottom", fontsize=8, color=INK_2)


def chart_histogram(stats, hists, out, configs, host, resolution):
    n = len(configs)
    fig, axes = plt.subplots(1, n, figsize=(5.2 * n, 4.3), squeeze=False, sharey=True)
    for ax, cfg in zip(axes[0], configs):
        h = hists[(hists["config"] == cfg) & (hists["median"] == 1)]
        for i, metric in enumerate(["total", "queue", "book"]):
            m = h[h["metric"] == metric]
            if m.empty:
                continue
            first, below, x, y = rebin(m, resolution)
            total = m["count"].sum()
            xs = [first * 0.75] + list(x)
            ys = [below / total] + list(y / total)
            pts = [(a, b) for a, b in zip(xs, ys) if b > 0]
            ax.plot([a for a, _ in pts], [b for _, b in pts], color=SERIES[i], label=metric,
                    linewidth=2 if metric == "total" else 1.4, zorder=3 if metric == "total" else 2,
                    linestyle="-" if metric != "queue" else (0, (4, 2)))
        ax.axvline(resolution, color=INK_2, linewidth=0.8, linestyle=":")
        ax.annotate("clock step", (resolution, 1), xytext=(3, -2), textcoords="offset points", fontsize=7.5,
                    color=INK_2, va="top")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_ylim(1e-9, 2)
        ax.xaxis.set_major_formatter(FuncFormatter(fmt_ns))
        ax.set_title(cfg)
        ax.set_xlabel("latency (log scale)")
        ax.grid(True, which="major")
        ax.grid(False, which="minor")
    axes[0][0].set_ylabel("fraction of messages per ⅒ decade (log)")
    axes[0][-1].legend(loc="upper right")
    fig.suptitle("Latency distribution of the median run (values under one clock step sit at the dotted line)",
                 x=0.01, ha="left", fontsize=13, fontweight="bold", color=INK)
    caption(fig, host)
    fig.tight_layout(rect=(0, 0.04, 1, 0.95))
    fig.savefig(out / "latency_histogram.png")
    plt.close(fig)


def bar_group(ax, labels, groups, values, value_fmt):
    """Grouped vertical bars: one group per label, one bar per entry in groups."""
    k = len(groups)
    width = 0.8 / k
    for gi, g in enumerate(groups):
        xs = [i + (gi - (k - 1) / 2) * width for i in range(len(labels))]
        vals = values[gi]
        bars = ax.bar(xs, vals, width=width * 0.92, color=SERIES[gi % len(SERIES)], label=g, zorder=3)
        for b, v in zip(bars, vals):
            ax.annotate(value_fmt(v), (b.get_x() + b.get_width() / 2, b.get_height()), xytext=(0, 2),
                        textcoords="offset points", ha="center", va="bottom", fontsize=7 if k > 2 else 7.5,
                        color=INK_2, rotation=90 if k > 2 else 0)
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels)
    ax.grid(axis="x", visible=False)


def chart_percentiles(stats, out, configs, host):
    med = median_rows(stats)
    labels = [p[1] for p in PCTS]
    values = [[med.loc[c, f"total_{p[0]}_ns"] for p in PCTS] for c in configs]
    fig, ax = plt.subplots(figsize=(10, 5))
    bar_group(ax, labels, configs, values, fmt_ns)
    ax.set_yscale("log")
    ax.set_ylim(top=ax.get_ylim()[1] * 8)  # room for rotated labels
    ax.yaxis.set_major_formatter(FuncFormatter(fmt_ns))
    ax.set_ylabel("end-to-end latency (log scale)")
    ax.set_title("Parser stamp → book updated: percentiles, median run")
    ax.legend(loc="upper left")
    caption(fig, host)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.savefig(out / "latency_percentiles.png")
    plt.close(fig)


def chart_breakdown(stats, out, configs, host):
    med = median_rows(stats)
    labels = [p[1] for p in PCTS[:4]] + ["mean"]
    keys = [p[0] for p in PCTS[:4]] + ["mean"]
    n = len(configs)
    fig, axes = plt.subplots(1, n, figsize=(5.2 * n, 4.4), squeeze=False)
    for ax, cfg in zip(axes[0], configs):
        values = [[med.loc[cfg, f"{stage}_{k}_ns"] for k in keys] for stage in ("queue", "book")]
        bar_group(ax, labels, ["queue wait", "book update"], values, fmt_ns)
        ax.set_yscale("symlog", linthresh=10)
        ax.yaxis.set_major_formatter(FuncFormatter(fmt_ns))
        ax.set_title(cfg)
    axes[0][0].set_ylabel("time per message (log scale)")
    axes[0][-1].legend(loc="upper left")
    fig.suptitle("Where the time goes: waiting in the ring vs applying to the book", x=0.01, ha="left",
                 fontsize=13, fontweight="bold", color=INK)
    caption(fig, host)
    fig.tight_layout(rect=(0, 0.04, 1, 0.95))
    fig.savefig(out / "latency_breakdown.png")
    plt.close(fig)


def chart_message_mix(stats, out, host):
    row = median_rows(stats).iloc[0]
    counts = {c[len("count_"):]: row[c] for c in stats.columns if c.startswith("count_") and c != "count_other"}
    total = float(row["file_messages"])
    shares = sorted(((v / total, k) for k, v in counts.items() if v > 0), reverse=True)
    top = shares[:8]
    rest = sum(s for s, _ in shares[8:]) + row.get("count_other", 0) / total
    labels = [f"{k}  {ITCH_NAMES.get(k, k)}" for _, k in top] + ["other types"]
    vals = [s * 100 for s, _ in top] + [rest * 100]
    fig, ax = plt.subplots(figsize=(8, 4.6))
    y = range(len(vals))[::-1]
    ax.barh(list(y), vals, color=SERIES[0], height=0.62, zorder=3)
    for yi, v in zip(y, vals):
        ax.annotate(f"{v:.2f}%", (v, yi), xytext=(4, 0), textcoords="offset points", va="center", fontsize=8.5,
                    color=INK_2)
    ax.set_yticks(list(y))
    ax.set_yticklabels(labels)
    ax.set_xlabel("share of all messages (%)")
    ax.grid(axis="y", visible=False)
    ax.set_xlim(0, max(vals) * 1.15)
    ax.set_title(f"ITCH message mix: {int(total):,} messages")
    caption(fig, host)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.savefig(out / "message_mix.png")
    plt.close(fig)


def chart_throughput(stats, out, configs, host):
    """Stability across the five runs: throughput where throughput is the
    result (unpaced), p99 latency where the rate is fixed (paced)."""
    unpaced = [c for c in configs if not stats[stats["config"] == c]["paced"].any()]
    paced = [c for c in configs if c not in unpaced]
    panels = [p for p in (("unpaced", unpaced), ("paced", paced)) if p[1]]
    fig, axes = plt.subplots(1, len(panels), figsize=(6.2 * len(panels), 4.4), squeeze=False)
    for ax, (kind, cfgs) in zip(axes[0], panels):
        for cfg in cfgs:
            i = configs.index(cfg)
            s = stats[stats["config"] == cfg].sort_values("run")
            y = s["book_msgs_per_sec"] / 1e6 if kind == "unpaced" else s["total_p99_ns"]
            med = y[s["median"].to_numpy() == 1].iloc[0]
            text = f"{cfg} (median {med:.2f} M/s)" if kind == "unpaced" else f"{cfg} (median {fmt_ns(med)})"
            ax.plot(s["run"], y, color=SERIES[i % len(SERIES)], marker="o", markersize=8, label=text,
                    markeredgecolor=SURFACE, markeredgewidth=2)
        ax.set_xlabel("run")
        ax.set_xticks(sorted(stats["run"].unique()))
        ax.set_xlim(0.7, stats["run"].max() + 0.3)
        if kind == "unpaced":
            ax.set_ylim(bottom=0, top=ax.get_ylim()[1] * 1.15)
            ax.set_ylabel("book messages per second (millions)")
            ax.set_title("Unpaced throughput per run")
            ax.legend(loc="lower left")
        else:
            ax.set_yscale("log")
            ax.set_ylabel("end-to-end p99 (log scale)")
            ax.yaxis.set_major_formatter(FuncFormatter(fmt_ns))
            ax.set_title("Paced p99 latency per run")
            ax.legend(loc="upper right")
    caption(fig, host)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.savefig(out / "throughput_runs.png")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", default="results", type=Path)
    ap.add_argument("--out", default="docs", type=Path)
    ap.add_argument("--configs", default=None, help="comma list of config dirs to plot, in order")
    args = ap.parse_args()
    style()
    stats, hists = load(args.results)
    configs = args.configs.split(",") if args.configs else config_order(stats)
    stats = stats[stats["config"].isin(configs)]
    hists = hists[hists["config"].isin(configs)]
    args.out.mkdir(parents=True, exist_ok=True)
    r = stats.iloc[0]
    res = float(r["clock_resolution_ns"]) if "clock_resolution_ns" in stats.columns else float(r["clock_ns_per_tick"])
    if "paced" not in stats.columns:
        stats["paced"] = stats["config"].str.startswith("paced")
    host = (f"{r['cpu_model']} · {r['os']} · {r['compiler'].split(' (')[0]} · clock {r['clock_source']}, "
            f"resolution {res:.1f} ns")
    chart_histogram(stats, hists, args.out, configs, host, res)
    chart_percentiles(stats, args.out, configs, host)
    chart_breakdown(stats, args.out, configs, host)
    chart_message_mix(stats, args.out, host)
    chart_throughput(stats, args.out, configs, host)
    for name in ["latency_histogram", "latency_percentiles", "latency_breakdown", "message_mix", "throughput_runs"]:
        print(f"wrote {args.out / (name + '.png')}")


if __name__ == "__main__":
    main()
