#!/usr/bin/env python3
"""Draw the README's throughput chart, light and dark, as two SVGs.

    python3 tools/plot_throughput.py docs/

The values are the README's Results table (medians, Apple M4); change them
here when that table changes. Standard library only.
"""

import sys
from pathlib import Path

GROUPS = [
    ("Synthetic mixed workload, 1M operations", [20.9, 35.7, 65.2]),
    ("20 minutes of Binance BTCUSDT, replayed", [14.7, None, 79.4]),
]
LABELS = ["v0  std::map", "v1  arrays + bitset", "v2  + id table"]
THEMES = {
    "light": dict(fg="#1f2328", mut="#59636e", grid="#d1d9e0", bars=["#af2b25", "#565b62", "#175fa4"]),
    "dark": dict(fg="#f0f6fc", mut="#9198a1", grid="#3d444d", bars=["#f2897c", "#65696f", "#79b6f4"]),
}
FONT = '-apple-system, BlinkMacSystemFont, "Segoe UI", "Noto Sans", Helvetica, Arial, sans-serif'
W, H, X0, XMAX, SCALE_MAX = 880, 414, 236, 800, 90


def svg(t):
    x = lambda v: X0 + v / SCALE_MAX * (XMAX - X0)
    alt = ("Throughput, million ops/s, Apple M4, one thread. Synthetic mixed workload: std::map 20.9M, "
           "arrays + bitset 35.7M, id table 65.2M. 20 minutes of Binance BTCUSDT replayed, a small book that fits in cache: "
           "std::map 14.7M, id table 79.4M.")
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         f'font-family=\'{FONT}\' role="img" aria-labelledby="t"><title id="t">{alt}</title>',
         f'<text x="0" y="28" font-size="24" font-weight="600" fill="{t["fg"]}">Throughput, million ops/s (higher is better)</text>',
         f'<text x="0" y="56" font-size="17" fill="{t["mut"]}">Apple M4, Apple clang 17 -O3, one thread; median of 10 (synthetic), 5 (replay)</text>']
    top, bottom = 82, 372
    for v in range(0, SCALE_MAX + 1, 20):
        p.append(f'<line x1="{x(v):.1f}" y1="{top}" x2="{x(v):.1f}" y2="{bottom}" stroke="{t["grid"]}" stroke-width="1"/>')
        p.append(f'<text x="{x(v):.1f}" y="{bottom + 22}" font-size="16" fill="{t["mut"]}" text-anchor="middle">{v}</text>')
    y = top + 26
    for title, values in GROUPS:
        p.append(f'<text x="0" y="{y}" font-size="18" font-weight="600" fill="{t["fg"]}">{title}</text>')
        y += 14
        for label, v, colour in zip(LABELS, values, t["bars"]):
            p.append(f'<text x="{X0 - 12}" y="{y + 16}" font-size="18" fill="{t["fg"]}" text-anchor="end" xml:space="preserve">{label}</text>')
            if v is None:
                p.append(f'<text x="{X0 + 6}" y="{y + 16}" font-size="18" fill="{t["mut"]}">not run</text>')
            else:
                p.append(f'<rect x="{X0}" y="{y}" width="{x(v) - X0:.1f}" height="22" fill="{colour}"/>')
                p.append(f'<text x="{x(v) + 8:.1f}" y="{y + 16}" font-size="18" font-weight="600" fill="{t["fg"]}">{v:g}M</text>')
            y += 32
        y += 22
    p.append("</svg>")
    return "\n".join(p) + "\n"


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "docs")
    for name, theme in THEMES.items():
        (out / f"throughput-{name}.svg").write_text(svg(theme))


if __name__ == "__main__":
    main()
