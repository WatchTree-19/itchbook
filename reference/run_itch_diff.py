#!/usr/bin/env python3
"""Differential test: C++ `replay --trace` against reference/itch_ref.py.

The C++ trace is streamed through a named pipe and compared line by line with
the lines the Python reference generates, so no trace is ever stored on disk.
Every line is a full top-of-book (price, shares, order count, both sides)
after one order book message, plus periodic full depth dumps with every
order's reference and shares in queue order. Exit status 0 means identical.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import itch_ref  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--replay", required=True, help="path to the replay binary")
    ap.add_argument("--input", required=True)
    ap.add_argument("--symbols", default="ALL")
    ap.add_argument("--checkpoint-every", type=int, default=0)
    ap.add_argument("--max-messages", type=int, default=None)
    ap.add_argument("--levels", default="vector")
    ap.add_argument("--workdir", default=None)
    a = ap.parse_args()

    workdir = a.workdir or tempfile.mkdtemp(prefix="itchdiff")
    os.makedirs(workdir, exist_ok=True)
    fifo = os.path.join(workdir, f"trace.{os.getpid()}.fifo")
    if os.path.exists(fifo):
        os.unlink(fifo)
    os.mkfifo(fifo)
    summary_path = os.path.join(workdir, f"replay_summary.{os.getpid()}.txt")

    cmd = [a.replay, "--trace", fifo, "--trace-symbols", a.symbols, "--levels", a.levels, "--times", "23:59:59"]
    if a.checkpoint_every:
        cmd += ["--checkpoint-every", str(a.checkpoint_every)]
    if a.max_messages is not None:
        cmd += ["--max-messages", str(a.max_messages)]
    cmd.append(a.input)

    t0 = time.time()
    with open(summary_path, "w") as summary:
        proc = subprocess.Popen(cmd, stdout=summary)
        syms = None if a.symbols == "ALL" else set(a.symbols.split(","))
        ref_lines = itch_ref.trace(a.input, syms, a.checkpoint_every, a.max_messages)
        n = tob = depth = 0
        mismatch = None
        with open(fifo, "r") as cpp:
            for want in ref_lines:
                got = cpp.readline()
                if got != want:
                    mismatch = (n, want, got)
                    break
                n += 1
                if want[0] == "D":
                    depth += 1
                else:
                    tob += 1
            if mismatch is None:
                extra = cpp.readline()
                if extra:
                    mismatch = (n, "<end of reference output>\n", extra)
            if mismatch is not None:
                proc.kill()
        rc = proc.wait()
    os.unlink(fifo)
    secs = time.time() - t0

    print(f"input            {a.input}")
    print(f"symbols          {a.symbols}")
    print(f"level container  {a.levels}")
    print(f"lines compared   {n} ({tob} top-of-book lines, {depth} depth lines)")
    print(f"elapsed          {secs:.1f} s")
    if mismatch is not None:
        i, want, got = mismatch
        print(f"MISMATCH at line {i}:\n  reference: {want.rstrip()}\n  replay:    {got.rstrip()}")
        return 1
    if n == 0:
        print("FAIL: nothing was compared")
        return 1
    # replay exits 1 if it saw anomalies; the trace can still match.
    print(f"replay exit status {rc} (summary in {summary_path})")
    print("IDENTICAL")
    return 0


if __name__ == "__main__":
    sys.exit(main())
