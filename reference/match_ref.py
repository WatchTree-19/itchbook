#!/usr/bin/env python3
"""Naive reference matching engine (price-time priority).

Deliberately simple: a dict per price level whose insertion order is the time
priority, a bisect-sorted list of prices per side, and a dict of live orders.
It implements the semantics documented in include/itchbook/matching.hpp and
prints exactly the event format of tools/match_replay, so the two outputs can
be compared line for line.
"""
import argparse
import bisect
import sys

MIN_PRICE, MAX_PRICE = 1, 1 << 20
RESERVED_ID = 2**64 - 1  # the C++ order map uses it as its empty marker


class Engine:
    def __init__(self, emit, lo=MIN_PRICE, hi=MAX_PRICE):
        self.emit = emit
        self.lo, self.hi = lo, hi
        self.levels = {"B": {}, "S": {}}  # price -> {id: qty}, oldest first
        self.prices = {"B": [], "S": []}  # ascending
        self.orders = {}  # id -> (side, price)

    def best(self, side):
        ps = self.prices[side]
        if not ps:
            return None
        return ps[-1] if side == "B" else ps[0]

    def level_event(self, side, price):
        q = self.levels[side].get(price)
        if q is None:
            self.emit(f"L {side} {price} 0 0\n")
        else:
            self.emit(f"L {side} {price} {sum(q.values())} {len(q)}\n")

    def drop_level_if_empty(self, side, price):
        if not self.levels[side][price]:
            del self.levels[side][price]
            ps = self.prices[side]
            ps.pop(bisect.bisect_left(ps, price))

    def remove(self, oid):
        side, price = self.orders.pop(oid)
        qty = self.levels[side][price].pop(oid)
        self.drop_level_if_empty(side, price)
        return side, price, qty

    def submit(self, oid, side, typ, tif, price, qty):
        if qty == 0:
            return self.emit(f"J {oid} Q\n")
        if typ == "L" and not (self.lo <= price <= self.hi):
            return self.emit(f"J {oid} P\n")
        if oid == RESERVED_ID:
            return self.emit(f"J {oid} R\n")
        if oid in self.orders:
            return self.emit(f"J {oid} D\n")
        self.incoming(oid, side, typ, tif, price, qty)

    def incoming(self, oid, side, typ, tif, price, qty):
        opp = "S" if side == "B" else "B"
        while qty > 0:
            bp = self.best(opp)
            if bp is None:
                break
            if typ == "L" and ((side == "B" and price < bp) or (side == "S" and price > bp)):
                break
            q = self.levels[opp][bp]
            while qty > 0 and q:
                mid = next(iter(q))
                fill = min(qty, q[mid])
                qty -= fill
                q[mid] -= fill
                self.emit(f"T {oid} {mid} {bp} {fill}\n")
                if q[mid] == 0:
                    del q[mid]
                    del self.orders[mid]
                self.emit(f"L {opp} {bp} {sum(q.values())} {len(q)}\n")
            self.drop_level_if_empty(opp, bp)
        if qty == 0:
            return
        if typ == "M":
            return self.emit(f"C {oid} {qty} M\n")
        if tif == "I":
            return self.emit(f"C {oid} {qty} I\n")
        lv = self.levels[side]
        if price not in lv:
            lv[price] = {}
            bisect.insort(self.prices[side], price)
        lv[price][oid] = qty
        self.orders[oid] = (side, price)
        self.emit(f"A {oid} {side} {price} {qty}\n")
        self.level_event(side, price)

    def cancel(self, oid):
        if oid not in self.orders:
            return self.emit(f"J {oid} U\n")
        side, price, qty = self.remove(oid)
        self.emit(f"C {oid} {qty} U\n")
        self.level_event(side, price)

    def replace(self, oid, price, qty):
        if oid not in self.orders:
            return self.emit(f"J {oid} U\n")
        if qty == 0:
            return self.emit(f"J {oid} Q\n")
        if not (self.lo <= price <= self.hi):
            return self.emit(f"J {oid} P\n")
        side, old_price = self.orders[oid]
        cur = self.levels[side][old_price][oid]
        if price == old_price and qty <= cur:
            self.levels[side][old_price][oid] = qty  # keeps its place in the dict
            self.emit(f"M {oid} {price} {qty}\n")
            return self.level_event(side, price)
        self.remove(oid)
        self.level_event(side, old_price)
        self.incoming(oid, side, "L", "D", price, qty)

    def dump(self, idx):
        self.emit(f"K {idx}\n")
        for side in ("B", "S"):
            ps = self.prices[side]
            for price in (reversed(ps) if side == "B" else ps):
                q = self.levels[side][price]
                ids = " ".join(f"{i}:{v}" for i, v in q.items())
                self.emit(f"{side} {price} {sum(q.values())} {len(q)} {ids}\n")


def run(lines, checkpoint_every=0):
    """Yields output lines for an iterable of input event lines."""
    buf = []
    eng = Engine(buf.append)
    idx = 0
    for line in lines:
        f = line.split()
        if f[0] == "N":
            eng.submit(int(f[1]), f[2], f[3], f[4], int(f[5]), int(f[6]))
        elif f[0] == "X":
            eng.cancel(int(f[1]))
        elif f[0] == "R":
            eng.replace(int(f[1]), int(f[2]), int(f[3]))
        else:
            raise ValueError(f"bad event line {idx}: {line!r}")
        idx += 1
        if checkpoint_every and idx % checkpoint_every == 0:
            eng.dump(idx)
        yield from buf
        buf.clear()
    if checkpoint_every:
        eng.dump(idx)
    yield from buf


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", nargs="?", default="-")
    ap.add_argument("--checkpoint-every", type=int, default=0)
    a = ap.parse_args()
    src = sys.stdin if a.input == "-" else open(a.input)
    for out in run(src, a.checkpoint_every):
        sys.stdout.write(out)


if __name__ == "__main__":
    main()
