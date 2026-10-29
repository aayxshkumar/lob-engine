#!/usr/bin/env python3
"""Plot latency CDFs from `lob_latency --csv` output as a standalone SVG.

    ./build/lob_latency 1000000 --csv quantiles.csv
    python3 tools/plot_cdf.py quantiles.csv add_limit latency-cdf.svg

No dependencies. The SVG carries its own light/dark styles, so it works as
an <img> on a page that follows prefers-color-scheme.
"""

import csv
import sys
from collections import defaultdict

TICK_NS = 1e9 / 24e6  # Apple Silicon steady_clock: 24 MHz, ~41.67 ns
X_MAX = 180           # ns shown on the x axis
W, H = 720, 360
L, R, T, B = 64, 24, 24, 56  # margins

SERIES = [
    ("v0_map", "v0: std::map levels", "s0"),
    ("v2", "v2: arrays + bitset + id table", "s1"),
]


def load(path, op):
    data = defaultdict(list)
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row["op"] == op:
                data[row["book"]].append((float(row["quantile"]), int(row["ns"])))
    return data


def x(ns):
    return L + (min(ns, X_MAX) / X_MAX) * (W - L - R)


def y(q):
    return T + (1 - q) * (H - T - B)


def step_path(points):
    # Empirical CDF: for each distinct latency value, the highest quantile at it.
    top = {}
    for q, ns in points:
        top[ns] = max(top.get(ns, 0.0), q)
    d = [f"M{x(0):.1f},{y(0):.1f}"]
    prev_q = 0.0
    for ns in sorted(top):
        if ns > X_MAX:
            break
        d.append(f"H{x(ns):.1f}V{y(top[ns]):.1f}")
        prev_q = top[ns]
    d.append(f"H{x(X_MAX):.1f}")
    return "".join(d), top


def main():
    src, op, out = sys.argv[1], sys.argv[2], sys.argv[3]
    data = load(src, op)

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" role="img" '
        f'aria-labelledby="t d" font-family="Charter, \'Bitstream Charter\', Cambria, serif" font-size="13">',
        f"<title id=\"t\">Latency CDF of {op}, v0 vs v2, Apple M4</title>",
        "<desc id=\"d\">Cumulative fraction of operations against latency. Both curves rise in "
        "steps every 41.7 ns because the M4's steady_clock ticks at 24 MHz.</desc>",
        "<style>"
        ".bg{fill:#fff}.ax{stroke:#111;stroke-width:1}.grid{stroke:#e5e5e5;stroke-width:1}"
        ".txt{fill:#111}.mut{fill:#555}.s0{stroke:#B3261E}.s1{stroke:#1F5FA8}"
        ".line{fill:none;stroke-width:2}"
        "@media (prefers-color-scheme: dark){.bg{fill:#111}.ax{stroke:#e8e8e8}.grid{stroke:#2a2a2a}"
        ".txt{fill:#e8e8e8}.mut{fill:#aaa}.s0{stroke:#F2857C}.s1{stroke:#7DB2F0}}"
        "</style>",
        f'<rect class="bg" width="{W}" height="{H}"/>',
    ]

    # Tick gridlines: one per clock tick.
    k = 0
    while k * TICK_NS <= X_MAX:
        gx = x(k * TICK_NS)
        parts.append(f'<line class="grid" x1="{gx:.1f}" y1="{T}" x2="{gx:.1f}" y2="{H - B}"/>')
        parts.append(f'<text class="mut" x="{gx:.1f}" y="{H - B + 18}" text-anchor="middle">{k * TICK_NS:.0f}</text>')
        k += 1
    for q in (0, 0.25, 0.5, 0.75, 1.0):
        parts.append(f'<line class="grid" x1="{L}" y1="{y(q):.1f}" x2="{W - R}" y2="{y(q):.1f}"/>')
        label = f"{q:.0%}"
        parts.append(f'<text class="mut" x="{L - 8}" y="{y(q) + 4:.1f}" text-anchor="end">{label}</text>')

    parts.append(f'<line class="ax" x1="{L}" y1="{H - B}" x2="{W - R}" y2="{H - B}"/>')
    parts.append(f'<line class="ax" x1="{L}" y1="{T}" x2="{L}" y2="{H - B}"/>')
    parts.append(
        f'<text class="txt" x="{(L + W - R) / 2:.0f}" y="{H - 14}" text-anchor="middle">'
        f"{op} latency, ns (gridlines = one 41.7 ns clock tick)</text>"
    )

    legend_y = T + 16
    for key, label, cls in SERIES:
        if key not in data:
            continue
        d, top = step_path(data[key])
        parts.append(f'<path class="line {cls}" d="{d}"/>')
        parts.append(f'<line class="line {cls}" x1="{W - R - 210}" y1="{legend_y - 4}" x2="{W - R - 186}" y2="{legend_y - 4}"/>')
        parts.append(f'<text class="txt" x="{W - R - 180}" y="{legend_y}">{label}</text>')
        legend_y += 20

    parts.append("</svg>")
    with open(out, "w") as f:
        f.write("\n".join(parts) + "\n")


if __name__ == "__main__":
    main()
