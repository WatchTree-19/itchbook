#!/usr/bin/env python3
"""Seeded random order flow for the matching engine differential test.

Output format (one event per line), read by tools/match_replay and
reference/match_ref.py:

  N <id> <B|S> <L|M> <D|I> <price> <qty>   new order
  X <id>                                   cancel
  R <id> <price> <qty>                     cancel/replace

The mix below is the default "deep" profile; "thin" (see PROFILES) shifts new
orders towards IOC and market orders. The mix is chosen to exercise every path, not to look like a real market:
about 50% resting-style limit orders placed around a drifting mid so that a
good share of them cross, 6% IOC, 3% market, 21% cancels (some of ids that are
already gone), 17% replaces (reduce, increase, reprice, crossing reprice) and
3% invalid requests (zero quantity, out-of-band price, duplicate live id,
the reserved id 2^64-1).
"""
import argparse
import random
import sys

MAX_PRICE = 1 << 20  # must match match_replay's default band [1, 2^20]


# Upper bounds of u for (day limit, IOC limit, market) among new orders.
# "deep" keeps thousands of orders resting; "thin" is dominated by aggressive
# flow, so the book is often empty on one side and market orders run out of
# liquidity (exercising the market remainder and empty-book paths).
PROFILES = {"deep": (0.50, 0.56, 0.59), "thin": (0.22, 0.40, 0.59)}


def generate(n, seed, out, profile="deep"):
    rng = random.Random(seed)
    p_day, p_ioc, p_new = PROFILES[profile]
    mid = 10000
    next_id = 1
    recent = []  # (id, side, price, qty) of recently submitted orders, some dead by now
    written = 0

    def w(line):
        nonlocal written
        out.write(line)
        written += 1

    def pick(recent):
        # Prefer recent orders that were placed passively (so are more likely
        # still resting); some picks still hit orders that have already traded.
        k = -1
        for _ in range(4):
            k = len(recent) - 1 - min(int(rng.expovariate(1 / 300.0)), len(recent) - 1)
            c = recent[k]
            if (c[1] == "B" and c[2] <= mid - 2) or (c[1] == "S" and c[2] >= mid + 2):
                break
        return k

    i = 0
    while written < n:
        i += 1
        if i % 200 == 0:
            mid = min(max(mid + rng.randint(-15, 15), 500), 20000)
        u = rng.random()
        if u < p_new:
            side = "B" if rng.random() < 0.5 else "S"
            qty = min(int(rng.lognormvariate(4.0, 1.0)) + 1, 5000)
            oid = next_id
            next_id += 1
            if u < p_day:  # day limit
                off = int(round(rng.gauss(4.0, 5.0)))
                price = mid - off if side == "B" else mid + off
                w(f"N {oid} {side} L D {max(price, 1)} {qty}\n")
                recent.append((oid, side, max(price, 1), qty))
            elif u < p_ioc:  # IOC limit, usually marketable
                off = rng.randint(-2, 6)
                price = mid + off if side == "B" else mid - off
                w(f"N {oid} {side} L I {max(price, 1)} {qty}\n")
            else:  # market
                w(f"N {oid} {side} M D 0 {qty}\n")
        elif u < 0.80:  # cancel
            if recent and rng.random() < 0.92:
                oid = recent.pop(pick(recent))[0]  # the generator will not cancel it twice
            else:
                oid = next_id + rng.randint(1, 1000)  # never used yet
            w(f"X {oid}\n")
        elif u < 0.97:  # replace
            if not recent:
                continue
            k = pick(recent)
            oid, side, price, qty = recent[k]
            mode = rng.random()
            if mode < 0.35:
                nq, np_ = rng.randint(1, qty), price  # reduce (or larger than what is left)
            elif mode < 0.55:
                nq, np_ = qty + rng.randint(1, 200), price  # increase
            elif mode < 0.85:
                nq, np_ = rng.randint(1, 2 * qty), max(1, price + rng.randint(-5, 5))  # reprice
            else:
                np_ = mid + rng.randint(0, 8) if side == "B" else max(1, mid - rng.randint(0, 8))  # aggressive
                nq = rng.randint(1, 2 * qty)
            w(f"R {oid} {np_} {nq}\n")
            recent[k] = (oid, side, np_, nq)
        else:  # invalid requests
            k = rng.randint(0, 6)
            if k == 0:
                w(f"N {next_id} B L D {mid} 0\n")
                next_id += 1
            elif k == 1:
                w(f"N {next_id} S L D {MAX_PRICE + rng.randint(1, 50)} 10\n")
                next_id += 1
            elif k == 2:
                w(f"N {next_id} B L D 0 10\n")
                next_id += 1
            elif k == 3 and recent:
                oid, side, price, qty = recent[-1]
                w(f"N {oid} {side} L D {price} {qty}\n")  # duplicate id if still live
            elif k == 4 and recent:
                w(f"R {recent[-1][0]} {recent[-1][2]} 0\n")
            elif k == 5:
                w(f"N {next_id} S M D 0 0\n")
                next_id += 1
            else:
                w(f"N {2**64 - 1} B L D {mid} 10\n")  # reserved id
        if len(recent) > 5000:
            del recent[:1000]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--n", type=int, default=1000000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default="-")
    ap.add_argument("--profile", default="deep", choices=sorted(PROFILES))
    a = ap.parse_args()
    out = sys.stdout if a.out == "-" else open(a.out, "w")
    generate(a.n, a.seed, out, a.profile)
    out.close()


if __name__ == "__main__":
    main()
