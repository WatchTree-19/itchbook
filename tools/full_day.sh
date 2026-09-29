#!/usr/bin/env bash
# Streams a full Nasdaq TotalView-ITCH 5.0 day from Nasdaq's sample server
# through the C++ replay and the Python reference at the same time, without
# storing the day on disk:
#
#   curl | tee (md5 of the .gz) | gunzip | tee (byte count)
#        | tee (replay: invariants, audits, volumes, top of book, trace -> sha256)
#        | itch_ref.py (trace -> sha256)
#
# The two traces (top of book after every order book message of the chosen
# symbols, plus full depth with every order in queue order every 10M messages)
# are hashed independently, which avoids coupling the two consumers, and
# compared at the end together with their line counts.
#
#   tools/full_day.sh [BUILD_DIR] [OUT_DIR]
# Environment: URL, SYMS (traced symbols), REPORT_SYMS, CSV_SYMS, PREFIX (a
# local raw prefix of the same day, e.g. the benchmark input; if the file
# exists its bytes are compared with the start of the stream, which shows it
# really is a prefix of this day's file).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/build/gcc}
OUT=${2:-$ROOT/../data/fullday}
URL=${URL:-https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz}
SYMS=${SYMS:-AAPL,MSFT,SPY,QQQ,AMZN,TSLA,NVDA,AMD,FB,INTC,CSCO,BAC,NFLX,IWM,GOOGL}
REPORT_SYMS=${REPORT_SYMS:-AAPL,MSFT,AMZN,TSLA,QQQ,NVDA,INTC,CSCO,NFLX,GOOGL,AMD,SPY}
CSV_SYMS=${CSV_SYMS:-AAPL}
CHECKPOINT=10000000
PREFIX=${PREFIX:-$ROOT/../data/prefix_raw.bin}

mkdir -p "$OUT"
rm -f "$OUT"/*.txt "$OUT"/*.sha256 "$OUT"/*.md5
if [ -f "$PREFIX" ]; then
  PREFIX_BYTES=$(stat -c %s "$PREFIX")
else
  PREFIX_BYTES=0
fi
start=$(date +%s)
curl -sS --fail "$URL" \
  | tee >(md5sum | cut -d' ' -f1 > "$OUT/gz.md5") \
  | gunzip \
  | tee >(wc -c > "$OUT/raw_bytes.txt") \
  | tee >(if [ "$PREFIX_BYTES" -gt 0 ]; then
            if cmp -s -n "$PREFIX_BYTES" - "$PREFIX"; then r=identical; else r=DIFFERENT; fi
            echo "$r ($PREFIX_BYTES bytes of $PREFIX)" > "$OUT/prefix_cmp.txt"
          else echo "no local prefix" > "$OUT/prefix_cmp.txt"; fi
          cat > /dev/null) \
  | tee >("$BUILD/replay" --symbols "$REPORT_SYMS" \
            --times 09:30:00,09:45:00,12:00:00,15:59:59,16:00:00,16:05:00 \
            --audit-every 50000000 \
            --trace >(sha256sum | cut -d' ' -f1 > "$OUT/cpp_trace.sha256") \
            --trace-symbols "$SYMS" --checkpoint-every $CHECKPOINT \
            --csv "$OUT/tob.csv" --csv-symbols "$CSV_SYMS" \
            - > "$OUT/replay_summary.txt"; echo $? > "$OUT/replay_exit.txt") \
  | python3 "$ROOT/reference/itch_ref.py" - --symbols "$SYMS" --checkpoint-every $CHECKPOINT \
      2> "$OUT/ref_stderr.txt" \
  | sha256sum | cut -d' ' -f1 > "$OUT/ref_trace.sha256"
# Process substitutions finish asynchronously; wait for their files.
for f in gz.md5 raw_bytes.txt prefix_cmp.txt cpp_trace.sha256 replay_exit.txt; do
  while [ ! -s "$OUT/$f" ]; do sleep 1; done
done
sleep 2
end=$(date +%s)

echo "elapsed           $((end - start)) s"
echo "gz md5            $(cat "$OUT/gz.md5")"
echo "raw bytes         $(cat "$OUT/raw_bytes.txt")"
echo "local prefix      $(cat "$OUT/prefix_cmp.txt")"
echo "replay exit       $(cat "$OUT/replay_exit.txt")"
grep -E "^(messages|trace lines written|trailing|RESULT)" "$OUT/replay_summary.txt"
cat "$OUT/ref_stderr.txt"
echo "cpp trace sha256  $(cat "$OUT/cpp_trace.sha256")"
echo "ref trace sha256  $(cat "$OUT/ref_trace.sha256")"
if [ "$(cat "$OUT/cpp_trace.sha256")" = "$(cat "$OUT/ref_trace.sha256")" ]; then
  echo "TRACES IDENTICAL"
else
  echo "TRACES DIFFER"
  exit 1
fi
