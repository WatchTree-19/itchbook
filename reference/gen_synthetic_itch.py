#!/usr/bin/env python3
"""Writes a synthetic TotalView-ITCH 5.0 file for CI.

Nasdaq's sample files are not licensed for redistribution, so CI uses this
generator instead. It has its own encoder (struct formats written from the
spec tables, independent of the C++ decoder), emits every message type in the
spec at least once, and drives a consistent order flow: executions, cancels,
deletes and replaces only ever reference live orders with at most their open
shares, so a correct book sees zero anomalies. Output is deterministic for a
given seed.
"""
import argparse
import random
import struct

# Formats after the common header. Header: type c, locate H, tracking H,
# timestamp 6 bytes (written separately).
FMT = {
    b"S": "c",                      # event code
    b"R": "8sccIcc2scccccIc",       # stock directory
    b"H": "8scc4s",                 # trading action
    b"Y": "8sc",                    # reg SHO
    b"L": "4s8sccc",                # market participant position
    b"V": "QQQ",                    # MWCB decline levels (Price(8))
    b"W": "c",                      # MWCB status
    b"K": "8sIcI",                  # IPO quoting period update
    b"J": "8sIIII",                 # LULD auction collar
    b"h": "8scc",                   # operational halt
    b"A": "Qc I8sI".replace(" ", ""),  # add order
    b"F": "QcI8sI4s",               # add order with MPID
    b"E": "QIQ",                    # executed
    b"C": "QIQcI",                  # executed with price
    b"X": "QI",                     # cancel
    b"D": "Q",                      # delete
    b"U": "QQII",                   # replace
    b"P": "QcI8sIQ",                # trade (non-cross)
    b"Q": "Q8sIQc",                 # cross trade
    b"B": "Q",                      # broken trade
    b"I": "QQc8sIIIcc",             # NOII
    b"N": "8sc",                    # RPII
    b"O": "8scIIIQII",              # DLCR
}
SIZES = {b"S": 12, b"R": 39, b"H": 25, b"Y": 20, b"L": 26, b"V": 35, b"W": 12, b"K": 28, b"J": 35, b"h": 21,
         b"A": 36, b"F": 40, b"E": 31, b"C": 36, b"X": 23, b"D": 19, b"U": 35, b"P": 44, b"Q": 40, b"B": 19,
         b"I": 50, b"N": 20, b"O": 48}


class Writer:
    def __init__(self, f):
        self.f = f
        self.count = 0
        self.track = 0

    def msg(self, typ, locate, ts, *fields):
        body = struct.pack(">" + FMT[typ], *fields)
        self.track = (self.track + 1) & 0xFFFF
        payload = typ + struct.pack(">HH", locate, self.track) + ts.to_bytes(6, "big") + body
        assert len(payload) == SIZES[typ], (typ, len(payload))
        self.f.write(struct.pack(">H", len(payload)) + payload)
        self.count += 1


def sym(s):
    return s.encode().ljust(8)


def generate(path, n_orders, seed):
    rng = random.Random(seed)
    names = ["SYNA", "SYNB", "SYNC", "SYND", "SYN E", "SYNFG"]
    with open(path, "wb") as f:
        w = Writer(f)
        ts = 3 * 3600 * 10**9

        def tick(lo=1, hi=700_000_000):
            nonlocal ts
            ts += rng.randint(lo, hi)
            return ts

        w.msg(b"S", 0, tick(), b"O")
        for loc, name in enumerate(names, start=1):
            w.msg(b"R", loc, tick(), sym(name), b"Q", b"N", 100, b"N", b"C", b"Z ", b"P", b"N", b"N", b"1", b"N", 0,
                  b"N")
        w.msg(b"V", 0, tick(), 300000000000, 280000000000, 250000000000)
        w.msg(b"S", 0, tick(), b"S")
        for loc, name in enumerate(names, start=1):
            w.msg(b"H", loc, tick(), sym(name), b"T", b" ", b"    ")
            w.msg(b"Y", loc, tick(), sym(name), b"0")
            w.msg(b"L", loc, tick(), b"NSDQ", sym(name), b"Y", b"N", b"A")
        w.msg(b"K", 5, tick(), sym(names[4]), 34200, b"A", 250000)
        w.msg(b"S", 0, tick(), b"Q")

        live = {}  # ref -> [loc, side, price, shares]
        pool = []  # live refs, for O(1) random choice
        pos = {}  # ref -> index in pool

        def track(ref, order):
            live[ref] = order
            pos[ref] = len(pool)
            pool.append(ref)

        def untrack(ref):
            del live[ref]
            i = pos.pop(ref)
            last = pool.pop()
            if last != ref:
                pool[i] = last
                pos[last] = i

        next_ref = 1000
        next_match = 1
        base = {loc: 100000 + 5000 * loc for loc in range(1, len(names) + 1)}
        mids = dict(base)
        adds = 0
        while adds < n_orders:
            loc = rng.randint(1, len(names))
            mid = mids[loc]
            if rng.random() < 0.01:  # mean-reverting mid, so the book never crosses
                mids[loc] = base[loc] + rng.randint(-300, 300)
            u = rng.random()
            # Keep a few thousand orders live: add more when the book is thin.
            p_add = 0.55 if len(live) < 3000 else 0.35
            if u < p_add or not live:
                side = b"B" if rng.random() < 0.5 else b"S"
                off = rng.randint(4, 60) * 100
                price = mid - off if side == b"B" else mid + off
                shares = rng.choice([1, 50, 100, 100, 100, 200, 300, 500, 1000, 2500])
                ref = next_ref
                next_ref += rng.randint(1, 4)
                if rng.random() < 0.15:
                    w.msg(b"F", loc, tick(), ref, side, shares, sym(names[loc - 1]), price, b"MPID")
                else:
                    w.msg(b"A", loc, tick(), ref, side, shares, sym(names[loc - 1]), price)
                track(ref, [loc, side, price, shares])
                adds += 1
                continue
            # Modify a random live order (of any symbol).
            u = rng.random()
            ref = pool[rng.randrange(len(pool))]
            o = live[ref]
            oloc = o[0]
            if u < 0.25:  # execute
                q = rng.randint(1, o[3])
                w.msg(b"E", oloc, tick(), ref, q, next_match)
                next_match += 1
                o[3] -= q
            elif u < 0.35:  # execute with price
                q = rng.randint(1, o[3])
                printable = b"Y" if rng.random() < 0.8 else b"N"
                w.msg(b"C", oloc, tick(), ref, q, next_match, printable, max(1, o[2] + rng.randint(-2, 2) * 100))
                next_match += 1
                o[3] -= q
            elif u < 0.50:  # partial or full cancel
                q = rng.randint(1, o[3])
                w.msg(b"X", oloc, tick(), ref, q)
                o[3] -= q
            elif u < 0.75:  # delete
                w.msg(b"D", oloc, tick(), ref)
                o[3] = 0
            else:  # replace: same price (loses priority) or a new one
                new_ref = next_ref
                next_ref += rng.randint(1, 4)
                off = rng.randint(4, 60) * 100
                new_price = o[2] if rng.random() < 0.3 else (mids[oloc] - off if o[1] == b"B" else mids[oloc] + off)
                new_shares = rng.choice([o[3], max(1, o[3] // 2), o[3] + 100])
                w.msg(b"U", oloc, tick(), ref, new_ref, new_shares, new_price)
                track(new_ref, [oloc, o[1], new_price, new_shares])
                o[3] = 0
            if o[3] == 0:
                untrack(ref)
            # Sprinkle the non-book messages through the day.
            r = rng.random()
            if r < 0.02:
                w.msg(b"P", loc, tick(), 0, b"B", rng.randint(1, 500), sym(names[loc - 1]), mid, next_match)
                next_match += 1
            elif r < 0.025:
                w.msg(b"I", loc, tick(), 1000, 200, b"B", sym(names[loc - 1]), mid, mid, mid, b"C", b"L")
            elif r < 0.027:
                w.msg(b"N", loc, tick(), sym(names[loc - 1]), b"A")
            elif r < 0.028:
                w.msg(b"B", loc, tick(), max(1, next_match - 1))
            elif r < 0.029:
                w.msg(b"J", loc, tick(), sym(names[loc - 1]), mid, mid + 500, mid - 500, 1)
            elif r < 0.0295:
                w.msg(b"h", loc, tick(), sym(names[loc - 1]), b"Q", b"H")
                w.msg(b"h", loc, tick(), sym(names[loc - 1]), b"Q", b"T")
            elif r < 0.030:
                w.msg(b"O", loc, tick(), sym(names[loc - 1]), b"N", mid - 1000, mid + 1000, mid, ts, mid - 100,
                      mid + 100)
        w.msg(b"W", 0, tick(), b"1")
        for loc, name in enumerate(names, start=1):
            w.msg(b"Q", loc, tick(), rng.randint(0, 100000), sym(name), mids[loc], next_match, b"C")
            next_match += 1
        w.msg(b"S", 0, tick(), b"M")
        w.msg(b"S", 0, tick(), b"E")
        w.msg(b"S", 0, tick(), b"C")
    return w.count


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True)
    ap.add_argument("--orders", type=int, default=60000)
    ap.add_argument("--seed", type=int, default=5)
    a = ap.parse_args()
    n = generate(a.out, a.orders, a.seed)
    print(f"wrote {n} messages to {a.out}")


if __name__ == "__main__":
    main()
