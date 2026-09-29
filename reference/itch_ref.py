#!/usr/bin/env python3
"""Naive reference order book for Nasdaq TotalView-ITCH 5.0.

Written to be obviously correct rather than fast, and independently of the C++
code: its own byte offsets (taken from the spec tables), plain dicts for
orders, a dict per price level whose insertion order is the FIFO queue, and a
bisect-sorted list of prices per side.

It produces the same trace format as `replay --trace`, so the two can be
compared line for line:

  <msg index> <locate> <result> <bid px> <bid shares> <bid orders> <ask px> <ask shares> <ask orders>

after every order book message (A F E C X D U) for the traced symbols, and,
every N messages and at the end, a full depth dump:

  D <messages so far> <locate> <B|S> <price> <shares> <orders> <ref>:<shares> ...

Result codes: 0 ok, 1 unknown_ref, 2 duplicate_ref, 3 over_execute,
4 over_cancel, 5 zero_shares, 6 bad_side, 7 locate_mismatch, 8 reserved_ref.
"""
import argparse
import bisect
import mmap
import struct
import sys

OK, UNKNOWN, DUPLICATE, OVER_EXEC, OVER_CANCEL, ZERO, BAD_SIDE, LOC_MISMATCH, RESERVED = range(9)
RESERVED_REF = 2**64 - 1  # the C++ order map uses it as its empty marker

H = struct.Struct(">H")
Q = struct.Struct(">Q")
I = struct.Struct(">I")


class Book:
    """All books. orders: ref -> [locate, side, price, shares]."""

    def __init__(self):
        self.orders = {}
        self.levels = {}  # (locate, side) -> {price: {ref: None}}  (dict order = FIFO)
        self.prices = {}  # (locate, side) -> sorted list of prices with orders

    def _level(self, loc, side, price, create):
        key = (loc, side)
        lv = self.levels.get(key)
        if lv is None:
            if not create:
                return None
            lv = self.levels[key] = {}
            self.prices[key] = []
        q = lv.get(price)
        if q is None and create:
            q = lv[price] = {}
            bisect.insort(self.prices[key], price)
        return q

    def _link(self, ref, loc, side, price, shares):
        self.orders[ref] = [loc, side, price, shares]
        self._level(loc, side, price, True)[ref] = None

    def _unlink(self, ref):
        loc, side, price, _ = self.orders.pop(ref)
        key = (loc, side)
        q = self.levels[key][price]
        del q[ref]
        if not q:
            del self.levels[key][price]
            ps = self.prices[key]
            ps.pop(bisect.bisect_left(ps, price))

    def add(self, loc, ref, side, shares, price):
        if side not in (66, 83):  # 'B', 'S'
            return BAD_SIDE
        if shares == 0:
            return ZERO
        if ref == RESERVED_REF:
            return RESERVED
        if ref in self.orders:
            return DUPLICATE
        self._link(ref, loc, side, price, shares)
        return OK

    def reduce(self, loc, ref, shares, over):
        o = self.orders.get(ref)
        if o is None:
            return UNKNOWN
        if o[0] != loc:
            return LOC_MISMATCH
        if shares > o[3]:
            return over
        o[3] -= shares
        if o[3] == 0:
            self._unlink(ref)
        return OK

    def delete(self, loc, ref):
        o = self.orders.get(ref)
        if o is None:
            return UNKNOWN
        if o[0] != loc:
            return LOC_MISMATCH
        self._unlink(ref)
        return OK

    def replace(self, loc, old, new, shares, price):
        o = self.orders.get(old)
        if o is None:
            return UNKNOWN
        if o[0] != loc:
            return LOC_MISMATCH
        if shares == 0:
            return ZERO
        if new == RESERVED_REF:
            return RESERVED
        if new != old and new in self.orders:
            return DUPLICATE
        side = o[1]
        self._unlink(old)
        self._link(new, loc, side, price, shares)  # new reference goes to the back
        return OK

    def side_top(self, loc, side):
        ps = self.prices.get((loc, side))
        if not ps:
            return 0, 0, 0
        price = ps[-1] if side == 66 else ps[0]
        q = self.levels[(loc, side)][price]
        return price, sum(self.orders[r][3] for r in q), len(q)

    def depth_lines(self, count, loc):
        out = []
        for side, ch in ((66, "B"), (83, "S")):
            ps = self.prices.get((loc, side)) or []
            order = reversed(ps) if side == 66 else ps
            for price in order:
                q = self.levels[(loc, side)][price]
                shares = [self.orders[r][3] for r in q]
                refs = " ".join(f"{r}:{s}" for r, s in zip(q, shares))
                out.append(f"D {count} {loc} {ch} {price} {sum(shares)} {len(q)} {refs}\n")
        return out


def chunks(path, size=1 << 24):
    """Yields (buffer, end) pairs of whole messages from a file or '-' (stdin).

    A file is memory mapped in one piece; a stream is read in chunks and any
    partial message at the end of a chunk is carried into the next one.
    """
    if path != "-":
        f = open(path, "rb")
        try:
            mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        except ValueError:  # empty file
            return
        yield mm, len(mm)
        return
    src = sys.stdin.buffer
    rest = b""
    while True:
        data = src.read(size)
        if not data:
            break
        buf = rest + data
        off = 0
        n = len(buf)
        while off + 2 <= n:
            ln = H.unpack_from(buf, off)[0]
            if off + 2 + ln > n:
                break
            off += 2 + ln
        yield buf, off
        rest = buf[off:]


def trace(path, symbols, checkpoint_every=0, max_messages=None):
    """Yields trace lines. symbols: set of names, or None for all."""
    book = Book()
    traced = set()
    idx = 0
    limit = max_messages if max_messages is not None else float("inf")
    for mm, n in chunks(path):
        off = 0
        while off + 2 <= n and idx < limit:
            ln = H.unpack_from(mm, off)[0]
            p = off + 2
            if p + ln > n:
                break
            off = p + ln
            t = mm[p]
            loc = H.unpack_from(mm, p + 1)[0]
            r = None
            if t == 82:  # 'R' stock directory: stock at 11, 8 bytes
                name = mm[p + 11:p + 19].decode("ascii").rstrip(" ")
                if symbols is None or name in symbols:
                    traced.add(loc)
            elif loc in traced:
                if t == 65 or t == 70:  # 'A' / 'F': ref 11, side 19, shares 20, stock 24, price 32
                    r = book.add(loc, Q.unpack_from(mm, p + 11)[0], mm[p + 19], I.unpack_from(mm, p + 20)[0],
                                 I.unpack_from(mm, p + 32)[0])
                elif t == 69 or t == 67:  # 'E' / 'C': ref 11, executed shares 19
                    r = book.reduce(loc, Q.unpack_from(mm, p + 11)[0], I.unpack_from(mm, p + 19)[0], OVER_EXEC)
                elif t == 88:  # 'X': ref 11, cancelled shares 19
                    r = book.reduce(loc, Q.unpack_from(mm, p + 11)[0], I.unpack_from(mm, p + 19)[0], OVER_CANCEL)
                elif t == 68:  # 'D': ref 11
                    r = book.delete(loc, Q.unpack_from(mm, p + 11)[0])
                elif t == 85:  # 'U': old ref 11, new ref 19, shares 27, price 31
                    r = book.replace(loc, Q.unpack_from(mm, p + 11)[0], Q.unpack_from(mm, p + 19)[0],
                                     I.unpack_from(mm, p + 27)[0], I.unpack_from(mm, p + 31)[0])
            if r is not None:
                bp, bs, bn = book.side_top(loc, 66)
                ap, as_, an = book.side_top(loc, 83)
                yield f"{idx} {loc} {r} {bp} {bs} {bn} {ap} {as_} {an}\n"
            idx += 1
            if checkpoint_every and idx % checkpoint_every == 0:
                for l in sorted(traced):
                    yield from book.depth_lines(idx, l)
        if idx >= limit:
            break
    if checkpoint_every:
        for l in sorted(traced):
            yield from book.depth_lines(idx, l)
    trace.messages = idx


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="raw ITCH file, or - for stdin")
    ap.add_argument("--symbols", default="ALL", help="comma separated, or ALL")
    ap.add_argument("--checkpoint-every", type=int, default=0)
    ap.add_argument("--max-messages", type=int, default=None)
    a = ap.parse_args()
    syms = None if a.symbols == "ALL" else set(a.symbols.split(","))
    out = sys.stdout
    lines = 0
    for line in trace(a.input, syms, a.checkpoint_every, a.max_messages):
        out.write(line)
        lines += 1
    out.flush()
    print(f"itch_ref: {trace.messages} messages, {lines} trace lines", file=sys.stderr)


if __name__ == "__main__":
    main()
