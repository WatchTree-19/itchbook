#!/usr/bin/env python3
"""Differential test: C++ matching engine against reference/match_ref.py.

Generates a seeded event file, runs tools/match_replay once per price-level
container (all in parallel, streamed through pipes) and compares every output
line (trades, level updates, rests, amends, cancels, rejects and a full book
dump with every order id and quantity in queue order every
--checkpoint-every events) with the Python reference. Exit status 0 means all
outputs were identical.
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_match_events  # noqa: E402
import match_ref  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--match-replay", required=True)
    ap.add_argument("--events", type=int, default=1000000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--profile", default="deep", choices=sorted(gen_match_events.PROFILES))
    ap.add_argument("--checkpoint-every", type=int, default=10000)
    ap.add_argument("--levels", default="vector,binary,map,ladder")
    ap.add_argument("--workdir", default=".")
    a = ap.parse_args()

    os.makedirs(a.workdir, exist_ok=True)
    events = os.path.join(a.workdir, f"events_{a.profile}_{a.seed}_{a.events}.txt")
    t0 = time.time()
    with open(events, "w") as f:
        gen_match_events.generate(a.events, a.seed, f, a.profile)

    procs = {}
    for lv in a.levels.split(","):
        cmd = [a.match_replay, "--levels", lv, "--checkpoint-every", str(a.checkpoint_every),
               "--audit-every", str(max(a.events // 20, 1)), events]
        procs[lv] = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                     bufsize=1 << 20)

    counts = {}
    n = 0
    mismatch = None
    with open(events) as src:
        for want in match_ref.run(src, a.checkpoint_every):
            key = want[0] + (want.split()[-1] if want[0] in "CJ" else "")
            counts[key] = counts.get(key, 0) + 1
            for lv, p in procs.items():
                got = p.stdout.readline()
                if got != want:
                    mismatch = (lv, n, want, got)
                    break
            if mismatch:
                break
            n += 1
    status = 0
    for lv, p in procs.items():
        if mismatch is None:
            extra = p.stdout.readline()
            if extra:
                mismatch = (lv, n, "<end of reference output>\n", extra)
        if mismatch is not None:
            p.kill()  # a broken engine may never finish; do not wait for it
        p.stdout.read()
        err = p.stderr.read()
        rc = p.wait()
        print(f"[{lv}] exit {rc}: {err.strip()}")
        if rc != 0:
            status = 1
    os.unlink(events)

    names = {"T": "trades", "L": "level updates", "A": "rests", "M": "amends", "CU": "user cancels",
             "CI": "IOC remainder cancels", "CM": "market remainder cancels", "JD": "rejects duplicate id",
             "JU": "rejects unknown id", "JQ": "rejects zero qty", "JP": "rejects price band",
             "JR": "rejects reserved id", "K": "checkpoints", "B": "bid level lines", "S": "ask level lines"}
    print(f"events           {a.events} (seed {a.seed}, profile {a.profile})")
    print(f"lines compared   {n} per container, containers: {a.levels}")
    print("  " + ", ".join(f"{names.get(k, k)} {v}" for k, v in sorted(counts.items())))
    print(f"elapsed          {time.time() - t0:.1f} s")
    if mismatch:
        lv, i, want, got = mismatch
        print(f"MISMATCH [{lv}] at line {i}:\n  reference:    {want.rstrip()}\n  match_replay: {got.rstrip()}")
        return 1
    if status:
        print("FAIL: match_replay reported an error")
        return 1
    print("IDENTICAL")
    return 0


if __name__ == "__main__":
    sys.exit(main())
