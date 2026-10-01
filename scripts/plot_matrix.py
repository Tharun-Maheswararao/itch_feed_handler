#!/usr/bin/env python3
"""Stage B comparison charts from the benchmark matrix.

    python3 scripts/plot_matrix.py --results results/matrix/c7i.2xlarge --out docs/matrix/c7i.2xlarge

Writes five PNGs:
    queue_percentiles.png   p50/p90/p99/p99.9/max for the four queues
    queue_cdf.png           latency CDF for the four queues (tail on a "nines" axis)
    alignment.png           padded vs unpadded indices: the cost of false sharing
    placement.png           same core vs different cores (vs different sockets)
    capacity.png            throughput and p99 by ring capacity
"""
import argparse
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402

from matrix_table import fmt_cap, load_matrix  # noqa: E402

# Colour follows the queue, in a fixed validated order.
QCOLOR = {"spsc": "#2a78d6", "rigtorp": "#eb6834", "boost": "#1baf7a", "mutex": "#eda100", "spsc_unaligned": "#e87ba4"}
QLABEL = {"spsc": "this repo (SPSC)", "rigtorp": "Rigtorp SPSCQueue", "boost": "boost::lockfree::spsc_queue",
          "mutex": "std::mutex + condvar", "spsc_unaligned": "SPSC, unpadded indices"}
FOUR = ["spsc", "rigtorp", "boost", "mutex"]
PLACES = ["same-core", "cross-core", "cross-socket", "unpinned"]
PCOLOR = {"same-core": "#2a78d6", "cross-core": "#eb6834", "cross-socket": "#1baf7a", "unpinned": "#52514e"}
SURFACE, INK, INK_2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"


def style():
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "axes.edgecolor": GRID, "axes.labelcolor": INK_2, "axes.titlecolor": INK, "axes.titlesize": 12,
        "axes.titleweight": "bold", "axes.titlelocation": "left", "xtick.color": INK_2, "ytick.color": INK_2,
        "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "axes.spines.top": False,
        "axes.spines.right": False, "legend.frameon": False, "legend.fontsize": 9, "font.size": 10,
        "lines.linewidth": 2, "figure.dpi": 110,
    })


def fmt_ns(v, _p=None):
    if v <= 0:
        return "0"
    if v < 1e3:
        return f"{v:.3g} ns"
    if v < 1e6:
        return f"{v / 1e3:.3g} µs"
    if v < 1e9:
        return f"{v / 1e6:.3g} ms"
    return f"{v / 1e9:.3g} s"


def caption(fig, text):
    fig.text(0.01, 0.01, text, ha="left", va="bottom", fontsize=8, color=INK_2)


def pick(values, preferred):
    for p in preferred:
        if p in values:
            return p
    return sorted(values)[0]


def cell(m, **kw):
    sel = m
    for k, v in kw.items():
        sel = sel[sel[k] == v]
    return sel.iloc[0] if len(sel) else None


def bars(ax, groups, series, value, color, label, fmt=fmt_ns, log=True):
    k = len(series)
    w = 0.8 / max(k, 1)
    for si, s in enumerate(series):
        xs = [gi + (si - (k - 1) / 2) * w for gi in range(len(groups))]
        vals = [value(g, s) for g in groups]
        b = ax.bar(xs, [v if v is not None else 0 for v in vals], width=w * 0.92, color=color(s), label=label(s), zorder=3)
        for rect, v in zip(b, vals):
            if v:
                ax.annotate(fmt(v), (rect.get_x() + rect.get_width() / 2, rect.get_height()), xytext=(0, 2),
                            textcoords="offset points", ha="center", va="bottom", fontsize=7, color=INK_2,
                            rotation=90 if k > 2 else 0)
    ax.set_xticks(range(len(groups)))
    ax.set_xticklabels(groups)
    ax.grid(axis="x", visible=False)
    if log:
        ax.set_yscale("log")
        ax.set_ylim(top=ax.get_ylim()[1] * 10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    style()
    df = load_matrix(args.results)
    m = df[df["median"] == 1]
    hist = pd.concat([pd.read_csv(p).assign(cell=p.parent.name) for p in args.results.glob("*/histogram.csv")])
    args.out.mkdir(parents=True, exist_ok=True)
    r0 = m.iloc[0]
    host = f"{r0['cpu_model']} · {r0['os']} · {r0['compiler'].split(' (')[0]} · median of runs · latency sampled 1/{int(r0['sample_every'])}"
    places = [p for p in PLACES if p in set(m.placement)]
    place = pick(places, ["cross-core", "same-core", "cross-socket", "unpinned"])
    caps = sorted(set(m.capacity))
    cap = pick(caps, [65536, 16384, 1024])
    loads = set(m.load)
    lat_load = "paced_itch" if "paced_itch" in loads else "unpaced"
    queues = [q for q in FOUR if q in set(m.queue)]

    # 1. Percentile bars, four queues
    fig, ax = plt.subplots(figsize=(10, 5))
    pcts = [("p50", "p50"), ("p90", "p90"), ("p99", "p99"), ("p999", "p99.9"), ("max", "max")]
    bars(ax, [p[1] for p in pcts], queues,
         lambda g, q: (lambda c: None if c is None else c[f"total_{dict((b, a) for a, b in pcts)[g]}_ns"])(
             cell(m, queue=q, capacity=cap, placement=place, load=lat_load)),
         lambda q: QCOLOR[q], lambda q: QLABEL[q])
    ax.yaxis.set_major_formatter(FuncFormatter(fmt_ns))
    ax.set_ylabel("end-to-end latency (log scale)")
    ax.set_title(f"Four queues, {lat_load}, {place}, {fmt_cap(cap)} slots: the tail gap is wider than the median gap", pad=26)
    ax.legend(loc="lower left", bbox_to_anchor=(0, 1.0), ncol=len(queues))
    caption(fig, host)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.savefig(args.out / "queue_percentiles.png")
    plt.close(fig)

    # 2. CDF with a "nines" y axis
    fig, ax = plt.subplots(figsize=(10, 5))
    for q in queues:
        c = cell(m, queue=q, capacity=cap, placement=place, load=lat_load)
        if c is None:
            continue
        h = hist[(hist.cell == c.cell) & (hist["median"] == 1) & (hist.metric == "total")].sort_values("bucket_lo_ns")
        cdf = h["count"].cumsum() / h["count"].sum()
        x = h["bucket_hi_ns"].to_numpy()
        y = -np.log10(np.clip(1 - cdf.to_numpy(), 1e-7, 1))
        ax.step(x, y, where="post", color=QCOLOR[q], label=QLABEL[q])
    ax.set_xscale("log")
    ax.xaxis.set_major_formatter(FuncFormatter(fmt_ns))
    ticks = [0.5, 0.9, 0.99, 0.999, 0.9999, 0.99999]
    ax.set_yticks([-math.log10(1 - t) for t in ticks])
    ax.set_yticklabels([f"{t * 100:g}%" for t in ticks])
    ax.set_ylim(0, -math.log10(1 - 0.99999) + 0.3)
    ax.set_xlabel("end-to-end latency (log scale)")
    ax.set_ylabel("fraction of messages at or below (nines scale)")
    ax.set_title(f"Latency CDF, {lat_load}, {place}, {fmt_cap(cap)} slots: how fast each queue reaches its slow cases")
    ax.legend(loc="upper left")
    caption(fig, host)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.savefig(args.out / "queue_cdf.png")
    plt.close(fig)

    # 3. Alignment: padded vs unpadded indices
    if "spsc_unaligned" in set(m.queue):
        fig, axes = plt.subplots(1, 2, figsize=(12, 4.6))
        pair = ["spsc", "spsc_unaligned"]
        if "unpaced" in loads:
            bars(axes[0], places, pair,
                 lambda p, q: (lambda c: None if c is None else c.book_msgs_per_sec / 1e6)(
                     cell(m, queue=q, capacity=cap, placement=p, load="unpaced")),
                 lambda q: QCOLOR[q], lambda q: QLABEL[q], fmt=lambda v: f"{v:.2f} M/s", log=False)
            axes[0].set_ylabel("book messages per second (millions)")
            axes[0].set_title(f"Unpaced throughput, {fmt_cap(cap)} slots")
            axes[0].set_ylim(top=axes[0].get_ylim()[1] * 1.15)
        bars(axes[1], places, pair,
             lambda p, q: (lambda c: None if c is None else c.total_p99_ns)(
                 cell(m, queue=q, capacity=cap, placement=p, load=lat_load)),
             lambda q: QCOLOR[q], lambda q: QLABEL[q])
        axes[1].yaxis.set_major_formatter(FuncFormatter(fmt_ns))
        axes[1].set_ylabel("end-to-end p99 (log scale)")
        axes[1].set_title(f"{lat_load} p99, {fmt_cap(cap)} slots")
        axes[0].legend(loc="lower left", bbox_to_anchor=(0, 1.03), ncol=2)
        fig.suptitle("False sharing: the same queue with and without cache-line padding", x=0.01, ha="left",
                     fontsize=13, fontweight="bold", color=INK)
        caption(fig, host)
        fig.tight_layout(rect=(0, 0.04, 1, 0.95))
        fig.savefig(args.out / "alignment.png")
        plt.close(fig)

    # 4. Placement
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.6))
    for ax, (k, name) in zip(axes, [("total_p50_ns", "p50"), ("total_p99_ns", "p99")]):
        bars(ax, queues, places,
             lambda q, p, k=k: (lambda c: None if c is None else c[k])(cell(m, queue=q, capacity=cap, placement=p, load=lat_load)),
             lambda p: PCOLOR[p], lambda p: p)
        ax.yaxis.set_major_formatter(FuncFormatter(fmt_ns))
        ax.set_title(f"{lat_load} {name}, {fmt_cap(cap)} slots")
        ax.set_xticklabels([QLABEL[q].split(" (")[0].replace("boost::lockfree::", "boost ") for q in queues], fontsize=8)
    axes[0].set_ylabel("end-to-end latency (log scale)")
    axes[0].legend(loc="lower left", bbox_to_anchor=(0, 1.03), ncol=len(places))
    fig.suptitle("Thread placement: where the two threads run", x=0.01, ha="left", fontsize=13, fontweight="bold", color=INK)
    caption(fig, host)
    fig.tight_layout(rect=(0, 0.04, 1, 0.95))
    fig.savefig(args.out / "placement.png")
    plt.close(fig)

    # 5. Capacity
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.6))
    for q in queues + (["spsc_unaligned"] if "spsc_unaligned" in set(m.queue) else []):
        for ax, load, val in [(axes[0], "unpaced", lambda c: c.book_msgs_per_sec / 1e6), (axes[1], lat_load, lambda c: c.total_p99_ns)]:
            pts = [(c, val(cell(m, queue=q, capacity=c, placement=place, load=load)))
                   for c in caps if cell(m, queue=q, capacity=c, placement=place, load=load) is not None]
            if pts:
                ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", markersize=7, color=QCOLOR[q],
                        label=QLABEL[q], markeredgecolor=SURFACE, markeredgewidth=1.5,
                        linestyle="--" if q == "spsc_unaligned" else "-")
    for ax in axes:
        ax.set_xscale("log", base=2)
        ax.set_xticks(caps)
        ax.set_xticklabels([fmt_cap(c) for c in caps])
        ax.set_xlabel("ring capacity (slots)")
    axes[0].set_ylim(bottom=0)
    axes[0].set_ylabel("book messages per second (millions)")
    axes[0].set_title(f"Unpaced throughput, {place}")
    axes[1].set_yscale("log")
    axes[1].yaxis.set_major_formatter(FuncFormatter(fmt_ns))
    axes[1].set_ylabel("end-to-end p99 (log scale)")
    axes[1].set_title(f"{lat_load} p99, {place}")
    axes[0].legend(loc="lower left", bbox_to_anchor=(0, 1.03), ncol=3)
    fig.suptitle("Ring capacity: where a bigger buffer stops helping", x=0.01, ha="left", fontsize=13, fontweight="bold", color=INK)
    caption(fig, host)
    fig.tight_layout(rect=(0, 0.04, 1, 0.95))
    fig.savefig(args.out / "capacity.png")
    plt.close(fig)
    for n in ["queue_percentiles", "queue_cdf", "alignment", "placement", "capacity"]:
        if (args.out / f"{n}.png").exists():
            print(f"wrote {args.out / (n + '.png')}")


if __name__ == "__main__":
    main()
