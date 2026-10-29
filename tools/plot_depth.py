#!/usr/bin/env python3
"""Draw a recording's resting depth as a heatmap: time across, price up, bids blue, asks red.

    python3 tools/plot_depth.py data/btcusdt.lobrec out.png 1584 396 [light|dark] [lo hi]

One row per $1 price bin between lo and hi (default: 350 dollars centred on
the first mid price), one column per slice of the recording, scaled to the
requested size. Colour intensity is (size / cap) ** 0.7, with cap at the
99.5th percentile of non-empty cells. Reads the LOBREC01 format written by
tools/record_binance.py. Needs Pillow.
"""

import struct
import sys

from PIL import Image

REC = struct.Struct("<QqIBBH")
THEMES = {
    "light": ("#ffffff", "#175fa4", "#af2b25"),
    "dark": ("#0d1013", "#79b6f4", "#f2897c"),
}


def rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def mix(a, b, t):
    return tuple(round(a[k] + (b[k] - a[k]) * t) for k in range(3))


def main():
    src, out, width, height = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    theme = sys.argv[5] if len(sys.argv) > 5 else "light"
    with open(src, "rb") as f:
        magic, price_scale, qty_scale = struct.unpack("<8sqq", f.read(24))
        data = f.read()
    if magic != b"LOBREC01":
        sys.exit("not a LOBREC01 file")
    n = len(data) // REC.size
    ts_first = REC.unpack_from(data, 0)[0]
    ts_last = max(REC.unpack_from(data, (n - 1) * REC.size)[0], ts_first + 1)

    bids, asks = {}, {}
    cols, lo = width, None
    hi = None
    grid = [dict() for _ in range(cols)]  # column -> {row: (side, size)}
    nxt = 0
    for i in range(n):
        ts, price, qty, kind, side, _ = REC.unpack_from(data, i * REC.size)
        if kind in (0, 1):
            book = bids if side == 0 else asks
            if qty == 0:
                book.pop(price, None)
            else:
                book[price] = qty
        elif kind == 3 and bids and asks:
            if lo is None:
                mid = (max(bids) + min(asks)) / 2 / price_scale
                lo = float(sys.argv[6]) if len(sys.argv) > 7 else round(mid) - 175
                hi = float(sys.argv[7]) if len(sys.argv) > 7 else lo + 350
            frac = (ts - ts_first) / (ts_last - ts_first)
            while nxt < cols and frac >= nxt / cols:
                cell = grid[nxt]
                for book, s in ((bids, 1), (asks, 2)):
                    for p, q in book.items():
                        dollars = p / price_scale
                        if lo <= dollars < hi:
                            r = int(dollars - lo)
                            cell[r] = (s, cell.get(r, (s, 0.0))[1] + q / qty_scale)
                nxt += 1
    for c in range(1, cols):
        if not grid[c]:
            grid[c] = grid[c - 1]

    rows = int(hi - lo)
    sizes = sorted(v for cell in grid for _, v in cell.values() if v > 0)
    cap = sizes[int(len(sizes) * 0.995)]
    bg, bid, ask = (rgb(h) for h in THEMES[theme])
    im = Image.new("RGB", (cols, rows), bg)
    px = im.load()
    for c, cell in enumerate(grid):
        for r, (s, v) in cell.items():
            t = 0.03 + 0.92 * min(1.0, v / cap) ** 0.7
            px[c, rows - 1 - r] = mix(bg, bid if s == 1 else ask, t)
    im.resize((width, height), Image.Resampling.BOX).save(out)
    print(f"{out}: {width}x{height}, ${lo:.0f} to ${hi:.0f}, cap {cap:.3f} BTC per $1 bin")


if __name__ == "__main__":
    main()
