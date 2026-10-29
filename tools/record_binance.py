#!/usr/bin/env python3
"""Record Binance spot depth diffs and trades to a binary file for lob_replay.

    python3 tools/record_binance.py BTCUSDT 600 data/btcusdt.lobrec

Standard library only (the websocket client is the ~40 lines below).

Follows Binance's procedure for a local order book: open the diff stream,
buffer events, fetch a REST snapshot, drop events the snapshot already
covers, then require every event's first update id to follow the previous
event's last one. A gap stops the recording rather than writing a book that
silently disagrees with the exchange.

File format, little-endian:
    header  8s magic "LOBREC01", q price_scale, q qty_scale
    record  Q ts_us, q price, I qty, B kind, B side, H pad     (24 bytes)
kind: 0 snapshot level, 1 diff level (qty is the new absolute level size,
0 removes it), 2 trade (side is the aggressor), 3 end of one diff message.
side: 0 buy/bid, 1 sell/ask. Prices are in ticks of 1/price_scale, sizes in
lots of 1/qty_scale.
"""

import base64
import json
import os
import socket
import ssl
import struct
import sys
import time
import urllib.request
from decimal import Decimal

HOST, PORT = "stream.binance.com", 9443
REST = "https://api.binance.com/api/v3/depth?symbol={}&limit=5000"
PRICE_SCALE, QTY_SCALE = 100, 100_000  # BTCUSDT: 0.01 tick, 0.00001 lot
REC = struct.Struct("<QqIBBH")
KIND_SNAPSHOT, KIND_DIFF, KIND_TRADE, KIND_END = 0, 1, 2, 3


class WebSocket:
    """Minimal RFC 6455 client: text frames in, pongs out."""

    def __init__(self, host, port, path):
        raw = socket.create_connection((host, port), timeout=30)
        self.sock = ssl.create_default_context().wrap_socket(raw, server_hostname=host)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(
            f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode()
        )
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            self.buf += self._recv()
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise RuntimeError(head.decode(errors="replace"))

    def _recv(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise ConnectionError("socket closed")
        return chunk

    def _take(self, n):
        while len(self.buf) < n:
            self.buf += self._recv()
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _send(self, opcode, payload):
        # Client frames must be masked.
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + masked)

    def recv_text(self):
        message = b""
        while True:
            b0, b1 = self._take(2)
            opcode, n = b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._take(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._take(8))[0]
            payload = self._take(n)  # server frames are unmasked
            if opcode == 0x9:
                self._send(0xA, payload)
            elif opcode == 0x8:
                raise ConnectionError("server closed the stream")
            elif opcode in (0x0, 0x1):
                message += payload
                if b0 & 0x80:
                    return message.decode()


def ticks(s):
    return int(Decimal(s) * PRICE_SCALE)


def lots(s):
    return min(int(Decimal(s) * QTY_SCALE), 0xFFFFFFFF)


def main():
    symbol, seconds, out_path = sys.argv[1].upper(), float(sys.argv[2]), sys.argv[3]
    s = symbol.lower()
    ws = WebSocket(HOST, PORT, f"/stream?streams={s}@depth@100ms/{s}@trade")

    # Buffer at least one diff before taking the snapshot, as Binance requires.
    buffered = []
    while not any(m["stream"].endswith("@depth@100ms") for m in buffered):
        buffered.append(json.loads(ws.recv_text()))
    with urllib.request.urlopen(REST.format(symbol), timeout=30) as r:
        snap = json.load(r)
    last_id = snap["lastUpdateId"]

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    counts = {"diff_msgs": 0, "diff_levels": 0, "trades": 0}
    with open(out_path, "wb") as f:
        f.write(struct.pack("<8sqq", b"LOBREC01", PRICE_SCALE, QTY_SCALE))
        now_us = int(time.time() * 1e6)
        for side, levels in ((0, snap["bids"]), (1, snap["asks"])):
            for p, q in levels:
                f.write(REC.pack(now_us, ticks(p), lots(q), KIND_SNAPSHOT, side, 0))

        prev_u = None
        deadline = time.time() + seconds

        def handle(m):
            nonlocal prev_u
            d = m["data"]
            if m["stream"].endswith("@trade"):
                if prev_u is None:
                    return  # before the book is synced
                side = 1 if d["m"] else 0  # buyer is maker -> the seller hit the bid
                f.write(REC.pack(d["T"] * 1000, ticks(d["p"]), lots(d["q"]), KIND_TRADE, side, 0))
                counts["trades"] += 1
                return
            if d["u"] <= last_id:
                return  # already in the snapshot
            if prev_u is None:
                if not d["U"] <= last_id + 1 <= d["u"]:
                    raise RuntimeError(f"first diff {d['U']}..{d['u']} does not follow snapshot {last_id}")
            elif d["U"] != prev_u + 1:
                raise RuntimeError(f"gap: expected {prev_u + 1}, got {d['U']}")
            prev_u = d["u"]
            ts = d["E"] * 1000
            for side, key in ((0, "b"), (1, "a")):
                for p, q in d[key]:
                    f.write(REC.pack(ts, ticks(p), lots(q), KIND_DIFF, side, 0))
                    counts["diff_levels"] += 1
            f.write(REC.pack(ts, 0, 0, KIND_END, 0, 0))
            counts["diff_msgs"] += 1

        for m in buffered:
            handle(m)
        while time.time() < deadline:
            handle(json.loads(ws.recv_text()))

    print(f"{symbol}: {len(snap['bids'])}+{len(snap['asks'])} snapshot levels, "
          f"{counts['diff_msgs']} diff messages ({counts['diff_levels']} level updates), "
          f"{counts['trades']} trades in {seconds:.0f} s -> {out_path}")


if __name__ == "__main__":
    main()
