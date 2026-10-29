#!/usr/bin/env python3
"""Plot latency CDFs from `lob_latency --csv` output as standalone SVGs, in clock ticks.

    ./build/lob_latency 1000000 --csv docs/m4-quantiles.csv
    python3 tools/plot_cdf.py docs/m4-quantiles.csv add_limit docs/latency-cdf-add_limit.svg
    python3 tools/plot_cdf.py docs/m4-quantiles.csv add_limit --json

Writes the wide chart to the named file and a narrow one next to it
(`*-narrow.svg`); --json prints the share of readings at or under 0..3 ticks
instead. Standard library only.

Every reading on Apple Silicon is a whole number of 41.67 ns ticks, but the
CSV stores integer nanoseconds, so one tick appears as both 41 and 42 ns (and
two as 83 and 84). Readings are binned with round(ns / tick); a plain
threshold (ns <= k * 41.67) would drop every 42 ns reading, and treating 41
and 42 as different values draws two steps per tick.
"""

import csv
import json
import sys
from collections import defaultdict
from pathlib import Path

TICK_NS = 1e9 / 24e6  # Apple Silicon steady_clock: 24 MHz, 41.67 ns
MAX_TICKS = 3
FONT = "Charter, 'Bitstream Charter', 'Sitka Text', Cambria, serif"
SERIES = [("v0_map", "v0", "s0"), ("v2", "v2", "s1")]
# Colours match the site: v0 red, v2 blue, ink and muted greys; dark values under the media query.
STYLE = (
    "<style>.ax{stroke:#171b20;stroke-width:1}.grid{stroke:#d5d8db;stroke-width:1}"
    ".txt{fill:#171b20}.mut{fill:#565b62}.s0{stroke:#af2b25}.s1{stroke:#175fa4}.t0{fill:#af2b25}.t1{fill:#175fa4}"
    ".line{fill:none;stroke-width:2}.s0.line{stroke-dasharray:5 3}"
    ".lbl{paint-order:stroke;stroke:#fff;stroke-width:3px;stroke-linejoin:round}"
    "@media (prefers-color-scheme: dark){.ax{stroke:#e5e8ec}.grid{stroke:#2f3338}"
    ".txt{fill:#e5e8ec}.mut{fill:#a7abb1}.s0{stroke:#f2897c}.s1{stroke:#79b6f4}.t0{fill:#f2897c}.t1{fill:#79b6f4}"
    ".lbl{stroke:#0d1013}}</style>"
)


def load(path, op):
    data = defaultdict(list)
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row["op"] == op:
                data[row["book"]].append((float(row["quantile"]), int(row["ns"])))
    return data


def shares(points):
    """Share of readings at or under k ticks, for k = 0..MAX_TICKS."""
    return [max((q for q, ns in points if round(ns / TICK_NS) <= k), default=0.0) for k in range(MAX_TICKS + 1)]


def chart(data, op, W, H, L, R, T, B, fs, long=True):
    x = lambda k: L + k / MAX_TICKS * (W - L - R)
    y = lambda q: T + (1 - q) * (H - T - B)
    s = {key: shares(data[key]) for key, _, _ in SERIES if key in data}
    summary = "; ".join(f"{label} reads 0 ticks for {s[key][0]:.1%} of calls" for key, label, _ in SERIES if key in s)
    p = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" role="img" '
        f'aria-labelledby="t" font-family="{FONT}" font-size="{fs}">',
        f'<title id="t">Latency CDF of {op} on an Apple M4, in clock ticks. {summary}.</title>',
        STYLE,
    ]
    for q in (0, 0.25, 0.5, 0.75, 1.0):
        p.append(f'<text class="mut" x="{L - 8}" y="{y(q) + 4:.1f}" text-anchor="end">{q:.0%}</text>')
    for k in range(MAX_TICKS + 1):
        p.append(f'<line class="grid" x1="{x(k):.1f}" y1="{T}" x2="{x(k):.1f}" y2="{H - B}"/>')
        p.append(f'<text class="txt" x="{x(k):.1f}" y="{H - B + fs + 5}" text-anchor="middle">{k}</text>')
        ns = "0" if k == 0 else f"{k * TICK_NS:.1f}" if long else f"{k * TICK_NS:.0f}"
        anchor = "end" if k == MAX_TICKS and not long else "middle"
        p.append(f'<text class="mut" x="{x(k):.1f}" y="{H - B + 2 * fs + 7}" text-anchor="{anchor}" '
                 f'font-size="{fs - 2}">{ns} ns</text>')
    p.append(f'<line class="ax" x1="{L}" y1="{H - B}" x2="{W - R}" y2="{H - B}"/>')
    p.append(f'<text class="txt" x="{(L + W - R) / 2:.0f}" y="{H - 6}" text-anchor="middle">'
             f'{op} reading, clock ticks at or under</text>')
    labels = []
    for key, label, cls in SERIES:
        if key not in s:
            continue
        v = s[key]
        d = f"M{x(0):.1f},{y(0):.1f}V{y(v[0]):.1f}" + "".join(f"H{x(k):.1f}V{y(v[k]):.1f}" for k in range(1, MAX_TICKS + 1))
        p.append(f'<path class="line {cls}" d="{d}"/>')
        text = f"{label}: {v[0]:.1%} at 0 ticks" if long else f"{label} {v[0]:.1%}"
        labels.append((x(0) + 8, y(v[0]) - 7, text))
        p.append(f'<text class="lbl t{cls[-1]}" x="{x(0) + 8:.1f}" y="{y(v[0]) - 7:.1f}">{text}</text>')
    p.append("</svg>")
    # Self-test: every direct label must end at least 6 px before the first riser at 1 tick.
    for lx, _, text in labels:
        right = lx + 0.52 * fs * len(text)
        if right > x(1) - 6:
            sys.exit(f"label {text!r} ends at {right:.0f} px, past the 1-tick riser at {x(1):.0f} px")
    return "\n".join(p) + "\n"


def main():
    src, op = sys.argv[1], sys.argv[2]
    data = load(src, op)
    if sys.argv[3] == "--json":
        print(json.dumps({label: [round(v * 100, 1) for v in shares(data[key])] for key, label, _ in SERIES if key in data}))
        return
    out = Path(sys.argv[3])
    out.write_text(chart(data, op, 720, 360, 70, 28, 24, 72, 16))
    narrow = out.with_name(out.stem + "-narrow" + out.suffix)
    narrow.write_text(chart(data, op, 360, 300, 46, 14, 16, 60, 13, long=False))
    for key, label, _ in SERIES:
        if key in data:
            print(label, " ".join(f"{k}:{v:.1%}" for k, v in enumerate(shares(data[key]))))


if __name__ == "__main__":
    main()
