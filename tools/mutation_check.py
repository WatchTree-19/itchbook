#!/usr/bin/env python3
"""Mutation check: plant realistic bugs one at a time and confirm the test
suite catches every one.

The source tree is copied to a scratch directory; each mutant is a single
text substitution in that copy. For each mutant the copy is rebuilt and the
checks below are run in order until one fails (the mutant is "killed"):

  1. unit_tests                         (doctest)
  2. ITCH differential on the synthetic file (reference/itch_ref.py)
  3. matching differential, 20,000 events (reference/match_ref.py)
  4. matching differential, 20,000 events of the "thin" profile

A mutant that survives all three, or that does not compile, is reported and
makes the script exit 1. Each check has a timeout, so a mutant that makes a
loop spin forever counts as killed by timeout.

With --all, every check is run for every mutant and the report lists all the
checks that caught it (slower, but it shows each layer's sensitivity on its
own, in particular that the differential tests catch bugs without help from the
unit tests).

  tools/mutation_check.py [--all] [--keep] [--only NAME[,NAME...]] [--compiler g++]
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (name, file, original text, mutated text). The original must occur in the
# file; the first occurrence is replaced.
MUTANTS = [
    ("parser: wrong byte offset (Add Order shares read at 21, not 20)",
     "include/itchbook/itch.hpp",
     "std::uint32_t shares() const noexcept { return be32(p + 20); }",
     "std::uint32_t shares() const noexcept { return be32(p + 21); }"),
    ("parser: endianness slip (be32 not byte-swapped)",
     "include/itchbook/itch.hpp",
     "if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap32(v);",
     ""),
    ("parser: 6-byte timestamp assembled with the wrong shift",
     "include/itchbook/itch.hpp",
     "return (std::uint64_t{be16(p)} << 32) | be32(p + 2);",
     "return (std::uint64_t{be16(p)} << 16) | be32(p + 2);"),
    ("parser: stock locate read one byte late",
     "include/itchbook/itch.hpp",
     "std::uint16_t stock_locate() const noexcept { return be16(p + 1); }",
     "std::uint16_t stock_locate() const noexcept { return be16(p + 2); }"),
    ("itch book: replace at the same price keeps queue priority",
     "include/itchbook/book.hpp",
     """        unlink(oi);
        map_.erase(old_ref);
        map_.insert(new_ref, oi);
        o.ref = new_ref;
        o.shares = shares;
        o.price = price;
        link(oi);""",
     """        map_.erase(old_ref);
        map_.insert(new_ref, oi);
        o.ref = new_ref;
        if (price == o.price) {
            levels_[o.level].shares = levels_[o.level].shares - o.shares + shares;
            o.shares = shares;
            return Result::ok;
        }
        unlink(oi);
        o.shares = shares;
        o.price = price;
        link(oi);"""),
    ("itch book: FIFO broken (new orders inserted at the head of the level)",
     "include/itchbook/book.hpp",
     """        o.next = kNil;
        o.prev = l.tail;
        if (l.tail != kNil) orders_[l.tail].next = oi; else l.head = oi;
        l.tail = oi;""",
     """        o.prev = kNil;
        o.next = l.head;
        if (l.head != kNil) orders_[l.head].prev = oi; else l.tail = oi;
        l.head = oi;"""),
    ("itch book: off-by-one in level removal (post-decrement)",
     "include/itchbook/book.hpp",
     "if (--l.count == 0) {",
     "if (l.count-- == 0) {"),
    ("itch book: partial execution does not reduce the level total",
     "include/itchbook/book.hpp",
     """            o.shares -= shares;
            levels_[o.level].shares -= shares;""",
     """            o.shares -= shares;"""),
    ("itch book: replace forgets to index the new reference",
     "include/itchbook/book.hpp",
     "        map_.insert(new_ref, oi);\n        o.ref = new_ref;",
     "        o.ref = new_ref;"),
    ("feed: partial cancel (X) treated as a full delete",
     "include/itchbook/feed.hpp",
     "return books.cancel(m.stock_locate(), m.order_reference_number(), m.cancelled_shares());",
     "return books.remove(m.stock_locate(), m.order_reference_number());"),
    ("feed: Order Executed With Price (C) ignored",
     "include/itchbook/feed.hpp",
     """    Result apply(const itch::OrderExecutedWithPrice& m) {
        return books.execute(m.stock_locate(), m.order_reference_number(), m.executed_shares());""",
     """    Result apply(const itch::OrderExecutedWithPrice& m) {
        (void)m;
        return Result::ok;"""),
    ("stream reader: partial message at the end of a read is dropped",
     "include/itchbook/feed.hpp",
     """        std::memmove(buf.data(), buf.data() + used, have - used);
        have -= used;""",
     """        have = 0;"""),
    ("hash map: backward-shift deletion boundary off by one",
     "include/itchbook/hash_map.hpp",
     "(i <= j) ? (home > i && home <= j) : (home > i || home <= j);",
     "(i <= j) ? (home >= i && home <= j) : (home >= i || home <= j);"),
    ("levels: vector binary search boundary off by one",
     "include/itchbook/levels.hpp",
     "if (better<Bid>(px, v_[mid].price)) lo = mid + 1; else hi = mid;",
     "if (!better<Bid>(v_[mid].price, px)) lo = mid + 1; else hi = mid;"),
    ("levels: ladder finds the wrong next ask after the best is removed",
     "include/itchbook/levels.hpp",
     "return static_cast<std::uint32_t>((w << 6) + static_cast<std::size_t>(std::countr_zero(word)) + min_);",
     "return static_cast<std::uint32_t>((w << 6) + static_cast<std::size_t>(std::countr_zero(word)) + min_ + 1);"),
    ("parser: Order Replace new shares read from the price field (offset 31, not 27)",
     "include/itchbook/itch.hpp",
     "std::uint32_t shares() const noexcept { return be32(p + 27); }",
     "std::uint32_t shares() const noexcept { return be32(p + 31); }"),
    ("levels: sorted vector reports the worst price as the best",
     "include/itchbook/levels.hpp",
     "[[nodiscard]] std::uint32_t best() const noexcept { return v_.empty() ? kNil : v_.back().idx; }",
     "[[nodiscard]] std::uint32_t best() const noexcept { return v_.empty() ? kNil : v_.front().idx; }"),
    ("itch book: reserved reference 2^64-1 accepted by add",
     "include/itchbook/book.hpp",
     "        if (ref == U64Map::kEmpty) [[unlikely]] return Result::reserved_ref;\n",
     ""),
    ("matching: market order does not sweep past the first level",
     "include/itchbook/matching.hpp",
     "if (count_after == 0) break;  // level is gone, lv is dangling",
     "if (count_after == 0) return qty;  // level is gone, lv is dangling"),
    ("matching: IOC limit remainder rests on the book",
     "include/itchbook/matching.hpp",
     "if (tif == TimeInForce::ioc) {",
     "if (tif == TimeInForce::ioc && type == OrderType::market) {"),
    ("matching: limit at exactly the opposite best price does not cross",
     "include/itchbook/matching.hpp",
     "if (type == OrderType::limit && better<MakerBid>(limit, px)) break;",
     "if (type == OrderType::limit && (better<MakerBid>(limit, px) || limit == px)) break;"),
    ("matching: time priority broken (new orders join the head of the level)",
     "include/itchbook/matching.hpp",
     """        o.next = kNil;
        o.prev = l.tail;
        if (l.tail != kNil) orders_[l.tail].next = oi; else l.head = oi;
        l.tail = oi;""",
     """        o.prev = kNil;
        o.next = l.head;
        if (l.head != kNil) orders_[l.head].prev = oi; else l.tail = oi;
        l.head = oi;"""),
    ("matching: trade printed at the taker's limit instead of the maker's price",
     "include/itchbook/matching.hpp",
     "l_.on_trade(TradeEvent{taker, maker_id, px, fill, taker_side});",
     "l_.on_trade(TradeEvent{taker, maker_id, limit, fill, taker_side});"),
    ("matching: replace with unchanged quantity loses priority",
     "include/itchbook/matching.hpp",
     "if (new_price == o.price && new_qty <= o.qty) {",
     "if (new_price == o.price && new_qty < o.qty) {"),
    ("matching: quantity increase keeps priority",
     "include/itchbook/matching.hpp",
     "if (new_price == o.price && new_qty <= o.qty) {\n            // Reduce (or no change) at the same price: keep queue position.\n            levels_[o.level].shares -= (o.qty - new_qty);",
     "if (new_price == o.price) {\n            // Reduce (or no change) at the same price: keep queue position.\n            levels_[o.level].shares = levels_[o.level].shares - o.qty + new_qty;"),
]


def run(cmd, cwd, timeout):
    # Each check runs in its own process group so that a timeout also kills
    # the binaries a driver script started (a mutant can make them spin).
    p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         start_new_session=True)
    try:
        out, _ = p.communicate(timeout=timeout)
        return p.returncode, out
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.communicate()
        return "timeout", ""


def checks(build, src):
    py = sys.executable
    return [
        ("unit_tests", [os.path.join(build, "unit_tests")], 300),
        ("itch diff (synthetic)", [py, os.path.join(src, "reference/run_itch_diff.py"), "--replay",
                                   os.path.join(build, "replay"), "--input",
                                   os.path.join(src, "tests/data/synthetic.itch"), "--symbols", "ALL",
                                   "--checkpoint-every", "20000", "--workdir", os.path.join(build, "diff")], 300),
        ("matching diff (20k events)", [py, os.path.join(src, "reference/run_match_diff.py"), "--match-replay",
                                        os.path.join(build, "match_replay"), "--events", "20000", "--seed", "11",
                                        "--checkpoint-every", "1000", "--workdir", os.path.join(build, "diff")], 300),
        ("matching diff thin (20k events)", [py, os.path.join(src, "reference/run_match_diff.py"), "--match-replay",
                                             os.path.join(build, "match_replay"), "--events", "20000", "--seed", "12",
                                             "--profile", "thin", "--checkpoint-every", "1000", "--workdir",
                                             os.path.join(build, "diff")], 300),
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep", action="store_true", help="keep the scratch directory")
    ap.add_argument("--all", action="store_true", help="run every check on every mutant")
    ap.add_argument("--only", default="", help="comma separated substrings of mutant names")
    ap.add_argument("--compiler", default="g++")
    a = ap.parse_args()

    scratch = tempfile.mkdtemp(prefix="itchbook-mut-")
    src = os.path.join(scratch, "src")
    build = os.path.join(scratch, "build")
    shutil.copytree(ROOT, src, ignore=shutil.ignore_patterns("build", ".git", "__pycache__"))
    rc, out = run(["cmake", "-S", src, "-B", build, "-G", "Ninja", f"-DCMAKE_CXX_COMPILER={a.compiler}",
                   "-DCMAKE_BUILD_TYPE=Release", "-DITCHBOOK_BENCH=OFF"], scratch, 300)
    if rc != 0:
        print(out)
        return 2
    targets = ["unit_tests", "replay", "match_replay"]

    # Baseline: every check must pass on the unmodified tree.
    rc, out = run(["ninja", "-C", build] + targets, scratch, 1800)
    if rc != 0:
        print(out)
        return 2
    for name, cmd, to in checks(build, src):
        rc, out = run(cmd, scratch, to)
        if rc != 0:
            print(f"baseline check failed: {name}\n{out[-3000:]}")
            return 2
    print("baseline: all checks pass on the unmodified tree\n")

    selected = [m for m in MUTANTS if not a.only or any(s in m[0] for s in a.only.split(","))]
    results = []
    for i, (name, rel, old, new) in enumerate(selected, 1):
        path = os.path.join(src, rel)
        original = open(path).read()
        if old not in original:
            results.append((name, "ERROR: original text not found"))
            print(f"[{i:2}/{len(selected)}] {name}: ERROR original text not found")
            continue
        open(path, "w").write(original.replace(old, new, 1))
        t0 = time.time()
        try:
            rc, out = run(["ninja", "-C", build] + targets, scratch, 1800)
            if rc != 0:
                verdict = "NOT COMPILED (mutant invalid)"
            else:
                verdict = "SURVIVED"
                caught = []
                for cname, cmd, to in checks(build, src):
                    rc, out = run(cmd, scratch, to)
                    if rc != 0:
                        caught.append(cname + (" (timeout)" if rc == "timeout" else ""))
                        if not a.all:
                            break
                if caught:
                    verdict = "killed by " + ", ".join(caught)
        finally:
            open(path, "w").write(original)
        results.append((name, verdict))
        print(f"[{i:2}/{len(selected)}] {name}: {verdict} ({time.time() - t0:.0f} s)", flush=True)

    # Rebuild the clean tree so the scratch build is left consistent.
    run(["ninja", "-C", build] + targets, scratch, 1800)
    killed = sum(1 for _, v in results if v.startswith("killed"))
    print(f"\n{killed}/{len(results)} mutants killed")
    if not a.keep:
        shutil.rmtree(scratch, ignore_errors=True)
    else:
        print(f"scratch kept at {scratch}")
    return 0 if killed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
