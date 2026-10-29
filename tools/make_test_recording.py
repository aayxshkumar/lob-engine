#!/usr/bin/env python3
"""Write a synthetic recording in the same format as tools/record_binance.py.

    python3 tools/make_test_recording.py build/synthetic.lobrec [messages] [seed]

CI uses it to run lob_replay under the sanitizers without committing exchange
data. The generator keeps its own consistent L2 book (bids always below
asks), so a replay of it should match the top of book after every diff.

Each message is either a batch of passive level changes or a trade that
takes size from the best level on one side; the diff that follows always
reports the new absolute size of every level it touched, as Binance does.
"""

import random
import struct
import sys

PRICE_SCALE, QTY_SCALE = 100, 100_000
REC = struct.Struct("<QqIBBH")
KIND_SNAPSHOT, KIND_DIFF, KIND_TRADE, KIND_END = 0, 1, 2, 3
BUY, SELL = 0, 1


def main():
    out = sys.argv[1]
    messages = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 7)

    mid = 8_000_000  # 80,000.00
    bids = {mid - 1 - i: rng.randint(1, 50) * 1000 for i in range(200)}
    asks = {mid + 1 + i: rng.randint(1, 50) * 1000 for i in range(200)}
    ts = 1_700_000_000_000_000

    with open(out, "wb") as f:
        f.write(struct.pack("<8sqq", b"LOBREC01", PRICE_SCALE, QTY_SCALE))
        for side, book in ((BUY, bids), (SELL, asks)):
            for p, q in sorted(book.items()):
                f.write(REC.pack(ts, p, q, KIND_SNAPSHOT, side, 0))

        for _ in range(messages):
            ts += 100_000
            touched = {}
            if rng.random() < 0.3 and bids and asks:
                # A trade: the aggressor takes part or all of the best opposite level.
                aggressor = BUY if rng.random() < 0.5 else SELL
                book, side = (asks, SELL) if aggressor == BUY else (bids, BUY)
                best = min(book) if side == SELL else max(book)
                take = rng.randint(1, book[best])
                f.write(REC.pack(ts, best, take, KIND_TRADE, aggressor, 0))
                book[best] -= take
                if book[best] == 0:
                    del book[best]
                touched[(side, best)] = book.get(best, 0)
            for _ in range(rng.randint(1, 6)):
                # Passive changes near the top: add size, cut size, or clear a level.
                side = BUY if rng.random() < 0.5 else SELL
                book = bids if side == BUY else asks
                best_bid = max(bids) if bids else mid - 1
                best_ask = min(asks) if asks else mid + 1
                if side == BUY:
                    p = rng.randint(best_bid - 30, min(best_bid + 2, best_ask - 1))
                else:
                    p = rng.randint(max(best_ask - 2, best_bid + 1), best_ask + 30)
                r = rng.random()
                if r < 0.55:
                    book[p] = book.get(p, 0) + rng.randint(1, 20) * 1000
                elif p in book and r < 0.85:
                    book[p] = rng.randint(0, book[p])
                    if book[p] == 0:
                        del book[p]
                elif p in book:
                    del book[p]
                touched[(side, p)] = book.get(p, 0)
            for (side, p), q in sorted(touched.items()):
                f.write(REC.pack(ts, p, q, KIND_DIFF, side, 0))
            f.write(REC.pack(ts, 0, 0, KIND_END, 0, 0))
            assert not bids or not asks or max(bids) < min(asks)

    print(f"wrote {messages} messages to {out}")


if __name__ == "__main__":
    main()
