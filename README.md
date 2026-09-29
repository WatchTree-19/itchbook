# itchbook

A C++20 limit order book toolkit built around Nasdaq TotalView-ITCH 5.0:

* `include/itchbook/itch.hpp`: zero-copy decoder for every message type in the ITCH 5.0 specification (23 types, including Operational Halt `h` and Direct Listing with Capital Raise `O`), template dispatch, no virtual calls.
* `include/itchbook/book.hpp`: order books rebuilt from ITCH, one per stock locate, FIFO queue per price level, O(1) best bid and ask, depth iteration.
* `include/itchbook/matching.hpp`: a separate exchange-style price-time priority matching engine (limit, market, IOC, cancel, cancel/replace), integer ticks, no allocation on the hot path.
* `tools/replay`: streams a raw ITCH file or pipe, builds every book, checks invariants and reports message counts, live and peak orders, traded volume and top of book.

The emphasis is on verification. The C++ is compared against deliberately naive Python references over a full trading day of real Nasdaq data and millions of random matching events, runs clean with gcc and clang under `-Werror` and under AddressSanitizer plus UndefinedBehaviorSanitizer, and a mutation check confirms that the tests catch 25 planted bugs. Benchmarks give the machine and the exact commands.

Header-only library, no dependencies. The only vendored file is `third_party/doctest/doctest.h` (MIT) for the unit tests.

## Contents

```
include/itchbook/
  itch.hpp          ITCH 5.0 message views, big-endian decoding, dispatch, framing
  book.hpp          ItchBooks: per-locate books rebuilt from ITCH, audit()
  matching.hpp      MatchingEngine: price-time priority engine with an event listener
  levels.hpp        price level containers: MapLevels, VectorLevels, VectorBinaryLevels, LadderLevels
  hash_map.hpp      open-addressing order map (linear probing, backward-shift deletion)
  pool.hpp          index-based object pool and the Level record
  huge_alloc.hpp    allocator that requests transparent huge pages for large buffers
  feed.hpp          ITCH-to-book glue, lookahead prefetching loop, streaming reader
tools/
  replay.cpp        full replay: invariants, volumes, top of book, CSV and trace output
  match_replay.cpp  runs the matching engine over an event file (for the differential test)
  full_day.sh       streams a whole Nasdaq day from Nasdaq's server through replay and the reference
  mutation_check.py plants 25 bugs one at a time and checks that the tests catch each one
reference/
  itch_ref.py       naive Python ITCH book producing the same trace as replay
  match_ref.py      naive Python matching engine producing the same event stream as match_replay
  run_itch_diff.py, run_match_diff.py   differential test drivers
  gen_match_events.py    seeded random order flow ("deep" and "thin" profiles)
  gen_synthetic_itch.py  synthetic ITCH generator with its own encoder (CI data)
tests/              doctest unit tests; tests/data/synthetic.itch (generated, 4.7 MB)
bench/              bench_itch, bench_matching, bench_memlat
```

## Build and test

Requirements: a C++20 compiler (tested with gcc 13.3.0 and clang 18.1.3), CMake 3.20+, Ninja, Python 3 (standard library only).

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `-DITCHBOOK_SANITIZE=ON` (`-fsanitize=address,undefined -fno-sanitize-recover=all`), `-DITCHBOOK_WERROR=ON` (default), `-DITCHBOOK_NATIVE=ON` (`-march=native`; off by default and off for every number below), `-DITCHBOOK_BENCH=OFF`. Every target is compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.

`ctest` runs five tests: the unit tests, two matching differential tests (200,000 events each, "deep" and "thin" flow), the ITCH differential test on the synthetic file (all symbols, full depth checkpoints), and a replay of the synthetic file that must report zero anomalies.

## Design

### Decoder

Each message type is a struct holding one pointer into the receive buffer; accessors decode a field only when called (`memcpy` plus byte swap; the 6-byte timestamp is a 2-byte and a 4-byte load). `itch::dispatch(p, len, handler)` switches on the type byte and calls `handler.on(Msg)` only for the types the handler has an overload for (checked with a `requires` expression), so there are no virtual calls and unused types cost nothing. A message whose length differs from the specification goes to `on_bad_length` instead of being decoded, and an unknown type goes to `on_unknown`. Offsets and sizes come from the Nasdaq TotalView-ITCH 5.0 specification (revision log up to February 19, 2026).

### ITCH book

* Orders live in a pool (`std::vector` plus free list) and are addressed by 32-bit index. An order is 32 bytes: reference, shares, price, FIFO links, level index, locate, side.
* The order reference to index lookup is an open-addressing hash map with linear probing, Fibonacci hashing and backward-shift deletion, so a day of inserts and deletes leaves no tombstones. It is sized up front for the expected peak and rehashes only if the load would pass 1/2 (the full day needed no rehash). The key 2^64-1 marks empty slots; the book rejects it explicitly (`reserved_ref`) instead of misbehaving.
* Each price level holds the aggregate shares, the order count and the head and tail of an intrusive doubly linked FIFO threaded through the orders. Levels also live in a pool.
* Each side of each book maps price to level through a pluggable container (below). Best bid and ask are O(1) for all of them.
* Large buffers (order map, pools) are allocated 2 MiB aligned and marked `MADV_HUGEPAGE`: the random accesses into a map of hundreds of MiB otherwise miss the TLB almost every time.

Semantics follow section 1.3 and 1.4 of the specification:

* Add (`A`, and `F` with MPID attribution, which is decoded but not stored): new order at the tail of its price level.
* Order Executed (`E`) and Order Executed With Price (`C`): reduce displayed shares; the order is removed at zero. A `C` does not move the order: it keeps its display price, the message price is the print price.
* Order Cancel (`X`): partial cancel, reduces displayed shares.
* Order Delete (`D`): removes the order.
* Order Replace (`U`): removes the original reference and adds the new reference with the original side and locate at the tail of the new price, so it always loses time priority, even at an unchanged price and size.

Messages the specification rules out (unknown reference, duplicate reference, executing or cancelling more shares than remain, zero shares, bad side, a locate that disagrees with the order, the reserved reference) are rejected with a `Result` code and leave the book unchanged, so a replay counts them instead of corrupting state. `audit()` walks every book and checks every structural invariant: level ordering, queue links in both directions, counts and share totals recomputed from the orders, every order indexed by its reference, pool and map sizes in agreement.

### Matching engine

`MatchingEngine<Listener, Levels>` keeps one instrument's book with the same pools, order map and level containers.

* Limit orders match against the opposite side best price first and oldest first within a price; every fill prints at the resting (maker) order's price. The remainder of a Day order rests; the remainder of an IOC order is cancelled.
* Market orders sweep price levels until filled or the book is empty; any remainder is cancelled, never rested.
* `replace(id, price, qty)`, where qty is the new open quantity: same price and a quantity not above the current open quantity amends in place and keeps queue position. A larger quantity or a different price removes the order and re-enters it as a new Day limit order with the same id, so it goes to the back of the queue and, at a new price, can trade immediately.
* Rejects leave the book unchanged: zero quantity, a limit price outside the configured band, the reserved id 2^64-1, a new order whose id is still live, cancel or replace of an id that is not live.
* Events go to the listener through optional member functions: `on_trade`, `on_level` (aggregate of a level after every change, zero when it disappears), `on_rest`, `on_amend`, `on_cancel` (user, IOC remainder, market remainder), `on_reject`. The event order for one request is fixed and documented in the header; the differential test checks it line by line.
* Prices are integer ticks and quantities integers. Orders, levels and the id map are preallocated; the benchmarks count allocations and report 0 during warm passes. Not implemented: self-trade prevention, hidden or iceberg orders, stops, several instruments per engine instance.

### Price level containers

| container | lookup | best price | notes |
|---|---|---|---|
| `MapLevels` | `std::map`, O(log n) | O(1) (`begin`) | one heap allocation per new level |
| `VectorLevels` | sorted vector with the best price at the back; linear scan of up to 8 entries from the best, then binary search | O(1) | inserting or erasing near the touch moves only the few better entries |
| `VectorBinaryLevels` | same vector, binary search only | O(1) | |
| `LadderLevels` | dense array indexed by price minus the band floor, plus an occupancy bitmap | O(1) | needs a bounded price band; when the best level empties, the next one is found by scanning bitmap words, which is slow if the book is sparse far from the touch |

Choice, from the measurements in the benchmark section:

* ITCH book: `VectorLevels`. Fastest measured on real data (8.67M msg/s median against 7.88M for binary search only and 4.62M for `std::map`, which also has twice the p99). It has no allocations once each book has reached its deepest depth and makes no assumption about the price range (ITCH prices run from $0.0001 to $200,000, so a ladder per book would need either a huge array or re-centering logic). ITCH activity concentrates near the touch, where the scan from the back of the vector finds a level in a few comparisons.
* Matching engine: `VectorLevels` is the default because it needs no price band. `LadderLevels` is 43% faster on the synthetic flow (21.85M against 15.28M requests/s, median) and is the better choice when the venue enforces a price band around the touch; its sparse-book worst case is described above.

## Verification

Data: `12302019.NASDAQ_ITCH50.gz` from `https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/`, the Nasdaq sample for Monday 30 December 2019 (3,524,013,057 bytes compressed, md5 `23514ef98821448a3f8f4c5b4095f3f5` computed while streaming; Nasdaq's `.md5sum` companion file returned 404, so that checksum could not be compared). Uncompressed it is 8,251,407,909 bytes and 268,744,780 messages. The date is confirmed by the closing prices below. The 1.6 GB prefix used for benchmarks and the all-symbol differential test was cut from the same stream; `tools/full_day.sh` compares it byte for byte with the start of the day (result: identical, all 1,599,999,972 bytes).

### Compilers and sanitizers

| build | unit tests | ctest (unit, 3 differential, replay invariants) |
|---|---|---|
| gcc 13.3.0, Release (`-O3`) | 154 test cases, 3,274,159 assertions, all pass | 5/5 pass |
| clang 18.1.3, Release (`-O3`) | 154 / 3,274,159, all pass | 5/5 pass |
| gcc 13.3.0, Debug, `-fsanitize=address,undefined` | 154 / 3,274,159, all pass | 5/5 pass |
| clang 18.1.3, Debug, `-fsanitize=address,undefined` | 154 / 3,274,159, all pass | 5/5 pass |

All with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`. The sanitizer builds use `-fno-sanitize-recover=all` and keep assertions enabled. Beyond `ctest`, the sanitizer builds also ran: the matching differential test at 1,000,000 events for each flow profile (both compilers, identical output), and `replay` over the 51,353,925-message real-data prefix with a full audit every 5M messages (both compilers: 0 anomalies, 11 audits, 0 failures, no sanitizer report).

Unit tests cover: each of the 23 message types decoded from hand-built buffers (offsets written as literals from the specification tables, bytes written by an independent big-endian writer, field values with distinct bytes so a wrong offset, width or byte order changes the result, plus one message spelled out byte by byte); wrong lengths, unknown types, framing with a truncated final message; the feed applier for every book message type (`A F E C X D U`, including that `C` leaves the order at its display price); every book operation and edge case (partial fills, filling exactly an order and exactly a level, replace at the same price losing priority, replace to a new price, over-execution and over-cancel rejected with the book unchanged, duplicate and reserved references, empty book, interior level removal) against all three ITCH containers; a 30,000-step randomised comparison with a brute-force model; every matching path (market into an empty book, market sweeping several levels, IOC remainder cancelled, fully filled IOC, time priority, limit at exactly the opposite price, band edges, every replace rule, every reject) against all four containers; the four level containers against each other under random churn; the order map under heavy collision churn and rehashing; the stream reader with 1 to 7 byte reads; lookahead dispatch against plain dispatch.

### Differential tests

**Matching engine against `reference/match_ref.py`.** `gen_match_events.py` produces seeded random flow; `run_match_diff.py` runs `match_replay` once per level container and compares every output line (trades, level updates, rests, amends, cancels, rejects, and a full book dump every 10,000 events with every order id and quantity in queue order) with the Python engine. The "deep" profile keeps thousands of orders resting; the "thin" profile is dominated by IOC and market orders, so the book is often empty on one side and market orders run out of liquidity.

| run | events | lines compared per container | containers | mismatches |
|---|---|---|---|---|
| gcc Release, deep, seed 1 | 2,000,000 | 4,628,431 | vector, binary, map, ladder | 0 |
| gcc Release, thin, seed 3 | 1,000,000 | 1,809,293 | vector, binary, map, ladder | 0 |
| clang and gcc ASan+UBSan, deep, seed 2 | 1,000,000 | 2,308,129 | vector, binary, map, ladder | 0 |
| clang and gcc ASan+UBSan, thin, seed 4 | 1,000,000 | 1,810,530 | vector, binary, map, ladder | 0 |

The deep seed 1 input was 1,042,150 Day limits, 119,955 IOC limits, 68,684 market orders, 420,161 cancels and 349,050 replaces. It produced 911,895 trades, 64,544 in-place amends, 203,411 user cancels, 55,099 IOC remainder cancels and 435,157 rejects (380,797 unknown id, mostly cancels and replaces of orders that had already traded; 22,982 zero quantity; 17,035 price out of band; 8,523 reserved id; 5,820 duplicate id). Market remainders do not occur in deep flow; the thin seed 3 run produced 49,467 market remainder cancels, 138,573 IOC remainder cancels and 389,815 trades.

**ITCH book against `reference/itch_ref.py`.** The reference has its own offsets and its own book (a dict of orders, a dict per price level whose insertion order is the FIFO, bisect-sorted prices). Both print, after every order book message (`A F E C X D U`) of the traced symbols, the result code and the full top of book (price, shares and order count, both sides), and periodically a full depth dump with every order's reference and shares in queue order.

| input | symbols | messages | top-of-book lines | depth lines | mismatches |
|---|---|---|---|---|---|
| real data prefix, 03:04:32 to 10:11:55 | all 8,906 | 51,353,925 | 49,815,313 | 4,347,518 (every 10M messages and at the end) | 0 |
| full day, streamed | 15 liquid symbols (1) | 268,744,780 | 16,454,343 lines in total (2) | | 0 (sha256 of both traces equal) |
| synthetic file (CI) | all | 140,897 | 228,447 lines in total | | 0 |

(1) AAPL, MSFT, SPY, QQQ, AMZN, TSLA, NVDA, AMD, FB, INTC, CSCO, BAC, NFLX, IWM, GOOGL. (2) Including depth dumps every 10M messages; for the full day the two traces are hashed independently (so the two consumers of one stream are not coupled) and the hashes and line counts compared.

The all-symbol comparison covers every book message in the first 19% of the day (through 10:11:55, 1.73M live orders at that point, past the opening cross and the first 40 minutes of regular trading). The full day is compared for the 15 symbols above.

### Full-day invariants

`tools/full_day.sh` streams `curl | gunzip | tee` into `replay` and the Python reference at the same time, so the day is never stored on disk. `replay` runs a full `audit()` of every book every 50M messages and at the end.

| check | full day result |
|---|---|
| messages processed | 268,744,780, all of them (8,251,407,909 bytes, 0 trailing bytes), 03:04:32.057 to 20:05:00.000 |
| orphans: E, C, X, D or U referencing an unknown order | 0 |
| duplicate order references on add or replace | 0 |
| executions or cancels larger than the order's remaining shares | 0 |
| zero-share adds, bad side codes, locate disagreeing with the order, reserved reference | 0 |
| messages with a length different from the specification, unknown types | 0 |
| timestamps going backwards | 0 |
| stock field in A, F, P, Q, H messages disagreeing with the stock directory | 0 |
| book messages for a locate never announced in the stock directory | 0 |
| full structural audits (every 50M messages and at the end) | 6 run, 0 failed |
| peak live orders | 1,924,078 |
| live orders and price levels after the End of Messages event | 0 and 0 |
| order map rehashes, pool reallocations | 0 and 0 |

"Orphans" are execute, cancel, delete or replace messages whose reference is not a live order. Sizes are unsigned and every subtraction is guarded by the over-execution and over-cancel checks, so a negative size would show up as one of those counters; the audits also recompute every level total and count from the individual orders.

Messages by type: A 117,145,568; D 114,360,997; U 21,639,067; E 5,722,824; I 4,024,315; X 2,787,676; F 1,485,888; P 1,218,602; L 215,161; C 99,917; Q 17,836; Y 9,013; H 8,966; R 8,906; S 6; J 34; K 3; V 1. No `B`, `W`, `h`, `N` or `O` messages occurred that day; those decoders are covered by the unit tests and the synthetic file.

Informational, not errors: some books are crossed (best bid above best ask) after a message: 8,579 book messages in the day (44 before 09:30, 8,371 during regular hours, 164 after 16:00; 2, 13 and 3 distinct symbols in those three periods), plus 70 that left a book locked. Nasdaq does not let a trading book stay crossed, so every case was classified:

* 7,945 were in a security whose last Trading Action was not `T` (halted, paused, or in an IPO quotation period). Such books accept orders without matching until the halt or IPO cross, so crossed interest is expected; for example IPDN, halted at the open.
* 623 were in trading state `T` immediately after a non-printable `C`. Nasdaq publishes a halt or IPO cross as the Cross Trade (`Q`, type `H`) and the Trading Action `T` first, then one non-printable execution per participating order; until the last one arrives the book shows the crossed pre-cross interest (for example SLGL at 10:10:57.018 after a LULD pause, and Nasdaq's test symbol ZWZZT at 10:00:22.633).
* 11 were in trading state `T` after an add, replace or delete: MKD (a new listing that day) three times, at 10:54:21.5696, 11:21:52.139 and 11:47:19.720, and MBOT at 14:20:41.2226. A separate scan of the raw stream shows each one between 0.18 and 0.45 ms after a halt cross (`Q` type `H`) for that symbol, with new orders interleaved among the cross's non-printable executions. These 11 are the only crossed states in trading state `T` outside the cross executions themselves, so each ends within a few messages.

The all-symbol differential test covers the cases before 10:11:55 (IPDN, ZWZZT, CETXP, SLGL), where the Python reference reproduces every one of these states message for message.

### External cross-checks

1. **Message counts against another implementation.** The independent `itch-book` project on PyPI ([pypi.org/project/itch-book](https://pypi.org/project/itch-book/)) publishes, for this file, 268,744,780 messages and 8.25 GB, and a latency table with 118.6M adds, 8.6M reduces (E/C/X), 114.4M deletes and 21.6M replaces. Here: 268,744,780 messages, 8,251,407,909 bytes, adds (`A`+`F`) 118,631,456, reduces 8,610,417, deletes 114,360,997, replaces 21,639,067. The total matches exactly and all four rounded counts agree. This checks framing and type decoding against another decoder, not against Nasdaq.

2. **Official closing prices.** For Nasdaq-listed securities the official close is the Nasdaq closing cross, printed in the `Q` message with cross type `C`. Published daily bars for 2019-12-30 were taken from Yahoo Finance's chart API, which reports split-adjusted prices; they are multiplied back by the splits Yahoo lists since that date (AAPL 4, AMZN 20, TSLA 15, NVDA 40, NFLX 10, GOOGL 20), using the unadjusted-for-dividends `close` field.

| symbol | ITCH closing cross | published close | ITCH opening cross | published open |
|---|---|---|---|---|
| AAPL | 291.52 | 291.52 | 289.44 | 289.46 |
| MSFT | 157.59 | 157.59 | 158.99 | 158.99 |
| AMZN | 1846.89 | 1846.89 | 1874.00 | 1874.00 |
| TSLA | 414.70 | 414.70 | 428.79 | 428.79 |
| QQQ | 212.21 | 212.21 | 213.50 | 213.50 |
| NVDA | 232.32 | 232.32 | 235.84 | 235.99 |
| INTC | 59.62 | 59.62 | 59.99 | 59.99 |
| CSCO | 47.59 | 47.59 | 47.75 | 47.75 |
| NFLX | 323.31 | 323.31 | 329.08 | 329.08 |
| GOOGL | 1339.71 | 1339.71 | 1356.81 | 1356.81 |
| AMD | 45.52 | 45.52 | 46.14 | 46.14 |

All 11 closes match to the cent. 9 of 11 opens match. The published open is the first regular-hours trade on any venue, not the Nasdaq opening cross (which for these symbols ran at about 09:30:00.8). For NVDA the first Nasdaq print of the session was a non-displayed execution (`P`) at 09:30:00.0076 at exactly 235.99, the published open. For AAPL the first Nasdaq print was 289.57 at 09:30:00.022, so the published 289.46 presumably printed on another venue, which Nasdaq data cannot show.

3. **Regular-session range.** `replay` reports, per symbol, the highest and lowest Nasdaq print between 09:30:00 and 16:00:01 (crosses included), over all printable executions and over prints of at least one round lot. Nasdaq prints are a subset of the consolidated tape, so the round-lot range must lie inside the published daily range. It does for all 12 symbols, and 6 of the 12 highs and 5 of the 12 lows are equal to the cent (Nasdaq is often where the extreme printed):

| symbol | Nasdaq high | published high | Nasdaq low | published low |
|---|---|---|---|---|
| AAPL | 292.69 | 292.69 | 285.23 | 285.22 |
| MSFT | 159.01 | 159.02 | 156.73 | 156.73 |
| AMZN | 1883.50 | 1884.00 | 1841.00 | 1840.62 |
| TSLA | 428.97 | 429.00 | 409.27 | 409.26 |
| QQQ | 213.63 | 213.63 | 211.16 | 211.16 |
| NVDA | 236.14 | 236.20 | 231.09 | 231.06 |
| INTC | 60.05 | 60.05 | 59.39 | 59.38 |
| CSCO | 47.88 | 47.88 | 47.29 | 47.29 |
| NFLX | 329.13 | 329.19 | 322.88 | 322.86 |
| GOOGL | 1356.81 | 1357.00 | 1338.01 | 1337.84 |
| AMD | 46.17 | 46.17 | 44.66 | 44.66 |
| SPY | 323.10 | 323.10 | 320.55 | 320.55 |

The round-lot filter matters. Including odd-lot prints, three symbols have Nasdaq prints outside the published range (AMZN low 1840.53, NFLX 329.20 and 322.85, GOOGL low 1337.63). Under the tape rules in force in 2019, odd-lot trades did not update the consolidated high and low, so this is expected. The check was first run without the filter, failed for these three symbols, and the filter was added for that reason.

What cannot be compared: daily volume (published volume is consolidated across all venues, this feed carries only Nasdaq executions: for example AAPL 7,330,969 printed shares on Nasdaq against 36,028,600 consolidated, 20.3%), and official prices of NYSE Arca listed SPY, which come from Arca's own auctions (SPY shows no Nasdaq opening or closing cross print).

### Mutation check

`tools/mutation_check.py` copies the tree, plants one bug at a time, rebuilds, and runs four checks: the unit tests, the synthetic ITCH differential test, and a 20,000-event matching differential test for each flow profile. With `--all` it runs every check on every mutant and reports all that caught it, which shows that the differential tests catch bugs on their own and not only the unit tests. Checks run in their own process group with a 300 s timeout, so a mutant that makes a binary spin counts as caught by timeout.

| # | planted bug | caught by |
|---|---|---|
| 1 | parser: wrong byte offset (Add Order shares read at 21, not 20) | unit, ITCH diff |
| 2 | parser: endianness slip (be32 not byte-swapped) | unit, ITCH diff |
| 3 | parser: 6-byte timestamp assembled with the wrong shift | unit |
| 4 | parser: stock locate read one byte late | unit, ITCH diff |
| 5 | itch book: replace at the same price keeps queue priority | unit, ITCH diff |
| 6 | itch book: FIFO broken (new orders inserted at the head of the level) | unit, ITCH diff |
| 7 | itch book: off-by-one in level removal (post-decrement) | unit, ITCH diff |
| 8 | itch book: partial execution does not reduce the level total | unit, ITCH diff |
| 9 | itch book: replace forgets to index the new reference | unit, ITCH diff |
| 10 | feed: partial cancel (X) treated as a full delete | unit, ITCH diff |
| 11 | feed: Order Executed With Price (C) ignored | unit, ITCH diff |
| 12 | stream reader: partial message at the end of a read is dropped | unit, ITCH diff |
| 13 | hash map: backward-shift deletion boundary off by one | unit |
| 14 | levels: vector binary search boundary off by one | unit, ITCH diff, match diff deep (timeout), match diff thin (timeout) |
| 15 | levels: ladder finds the wrong next ask after the best is removed | unit, match diff deep, match diff thin |
| 16 | parser: Order Replace new shares read from the price field (offset 31, not 27) | unit, ITCH diff |
| 17 | levels: sorted vector reports the worst price as the best | unit, ITCH diff, match diff deep, match diff thin |
| 18 | itch book: reserved reference 2^64-1 accepted by add | unit |
| 19 | matching: market order does not sweep past the first level | unit, match diff deep, match diff thin |
| 20 | matching: IOC limit remainder rests on the book | unit, match diff deep, match diff thin |
| 21 | matching: limit at exactly the opposite best price does not cross | unit, match diff deep, match diff thin |
| 22 | matching: time priority broken (new orders join the head of the level) | unit, match diff deep, match diff thin |
| 23 | matching: trade printed at the taker's limit instead of the maker's price | unit, match diff deep, match diff thin |
| 24 | matching: replace with unchanged quantity loses priority | unit, match diff deep, match diff thin |
| 25 | matching: quantity increase keeps priority | unit, match diff deep, match diff thin |

Result: 25 of 25 killed. The unit tests catch all 25; the differential tests on their own catch 22. The three they miss: the timestamp shift and the reserved reference (neither changes book state in the generated inputs) and the hash map deletion boundary (the synthetic ITCH file and the 20,000-event matching flows never build the colliding probe runs it needs; the unit test that builds them catches it).

## Benchmarks

Machine: a KVM cloud VM with 2 vCPUs, reported as `Intel(R) Xeon(R) Processor @ 2.10GHz`, family 6 model 207 (the Emerald Rapids generation; the hypervisor hides the product name), 4 MiB L2 per core, 7 GB RAM, Linux 6.18, transparent huge pages in `madvise` mode, invariant TSC (`constant_tsc`, `nonstop_tsc`) at 2.1 GHz. gcc 13.3.0, `-O3`, no `-march=native`. Each benchmark pins itself to CPU 1 and prints the load average at start and end; the runs below were taken with nothing else running (load average 1.00 at the end, which is the benchmark itself). The VM exposes no hardware performance counters, so there are no cache-miss counts.

Method: per-message latency is timed with `lfence; rdtsc` before and `rdtscp; lfence` after each message, with the TSC calibrated against `steady_clock` over 500 ms (2.1000 ticks/ns). The fences serialise execution, so every per-message figure includes about 21 ns of timer overhead and no overlap between consecutive messages. Throughput passes run without per-message timing and are the better guide to sustained cost. The engines' results are consumed (result counters, a sink that sums trades and level quantities, checked against the first pass) so the work cannot be optimised away. Allocations are counted by replacing global `operator new`.

### Memory latency floor (`bench_memlat`)

Dependent random loads, one per cache line, ns per load:

| buffer | 4 KiB pages | transparent huge pages |
|---|---|---|
| 1 MiB | 7.0 | 5.4 |
| 4 MiB | 36.4 | 33.8 |
| 16 MiB | 50.6 | 34.6 |
| 64 MiB | 127.8 | 129.5 |
| 256 MiB | 136.9 | 115.7 |
| 1024 MiB | 152.6 | 117.2 |

A miss to DRAM costs 115 to 150 ns here. The ITCH replay keeps 1.7M live orders in a 128 MiB order map plus a 55 MiB order pool, so its cost is dominated by a few dependent cache misses per message rather than by instructions.

### ITCH parse plus book update (`bench_itch`)

Input: the first 1,599,999,972 bytes of the day (51,353,925 messages, 03:04:32 to 10:11:55, 1,729,408 live orders at the end, peak 1,729,764) held in memory. Each container gets one cold pass, 5 warm throughput passes (books cleared between passes, capacity kept), 5 passes each with lookahead prefetching, and one latency pass.

| level container | warm throughput, median of 5 | best of 5 | with lookahead 4, best | allocations per warm pass | p50 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|
| `VectorLevels` (chosen) | 8.67M msg/s | 9.16M msg/s, 109.1 ns/msg | 10.89M msg/s, 91.8 ns/msg | 0 | 169.5 | 607.6 | 912.4 |
| `VectorBinaryLevels` | 7.88M msg/s | 8.04M msg/s, 124.4 ns/msg | 9.62M msg/s | 0 | 175.2 | 628.6 | 930.5 |
| `MapLevels` (`std::map`) | 4.62M msg/s | 4.77M msg/s, 209.6 ns/msg | 5.36M msg/s | 10,427,190 | 222.9 | 1,196.2 | 2,573.3 |
| `VectorLevels`, identity-hashed order map | 8.33M msg/s | 8.57M msg/s, 116.7 ns/msg | 10.24M msg/s | 0 | 145.7 | 582.9 | 5,796.2 |

Latency columns are per message in ns, all message types, timer overhead included. Per message type with `VectorLevels`:

| message | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|---|
| all | 51,353,925 | 212.7 | 169.5 | 366.7 | 607.6 | 912.4 | 21,500 | 3,118,641 |
| A add | 21,831,147 | 201.1 | 163.8 | 298.1 | 461.9 | 669.5 | 20,814 | 3,118,641 |
| F add with MPID | 987,211 | 201.6 | 163.8 | 306.7 | 467.6 | 675.2 | 20,573 | 468,709 |
| D delete | 20,436,691 | 216.6 | 179.0 | 384.8 | 621.0 | 896.2 | 21,715 | 2,268,942 |
| U replace | 4,642,088 | 341.2 | 301.0 | 525.7 | 781.9 | 3,084.8 | 25,908 | 2,848,156 |
| E executed | 868,970 | 207.4 | 165.7 | 370.5 | 600.0 | 856.2 | 23,269 | 1,752,048 |
| X cancel | 1,032,078 | 96.3 | 59.0 | 199.0 | 422.9 | 628.6 | 12,995 | 734,582 |
| C executed with price | 17,128 | 174.2 | 125.7 | 306.7 | 550.5 | 811.4 | 22,235 | 115,412 |
| non-book types | 1,538,612 | 26.3 | 23.8 | 32.4 | 40.0 | 118.1 | 337.1 | 47,231 |

Reading the numbers:

* `VectorLevels` against `std::map`: 1.9x the throughput, half the p99, and no allocations (the map allocates a node for every new price level, 10.4M times per pass). Against the binary-search-only vector: 10% more throughput, because most book activity lands at or within a few levels of the touch, where the short scan from the back of the vector finds the level without a binary search.
* Replace is the most expensive message (an unlink, a re-key of the order map and a link, often at a new level). Cancel is the cheapest book message because a partial cancel never touches the level list.
* The p99.99 values (13 to 26 us) appear for every book message type but not for the 24 ns non-book messages. That pattern fits interruptions that arrive at a fixed rate in time (timer ticks, hypervisor exits): a 200 ns message is about 8 times as likely as a 25 ns one to contain one. The max values (milliseconds) are single events of the same kind. They are properties of the VM, not of the code, and are left in rather than filtered.
* Identity hashing of Nasdaq's increasing order references gives a lower median (145.7 against 169.5 ns) but a p99.9 more than 6 times worse (5,796 against 912 ns) and lower throughput, consistent with long linear-probe runs forming where consecutive references fill consecutive slots. The default is therefore Fibonacci hashing.
* "Lookahead" is `dispatch_buffer_lookahead<Step>`: while processing message i it prefetches the order map slot of message i+3*Step, the order of message i+2*Step and the level and queue neighbours of message i+Step. It gains 19% throughput with `VectorLevels` by overlapping cache misses of different messages. It only applies when messages are available ahead of time (file replay, or the messages of one network packet); the latency figures do not use it.

### Matching engine (`bench_matching`)

Input: the 2,000,000-event deep seed 1 flow from the differential test (`gen_match_events.py --n 2000000 --seed 1`), parsed into memory first. Each pass takes about 0.1 s, so 25 warm passes are run and the median is the headline figure.

| level container | warm throughput, median of 25 | best of 25 | allocations per warm pass | p50 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| `LadderLevels` | 21.85M req/s, 45.8 ns/req | 24.48M req/s, 40.8 ns/req | 0 | 92.4 | 368.6 | 521.9 |
| `VectorLevels` (default) | 15.28M req/s, 65.4 ns/req | 16.44M req/s, 60.8 ns/req | 0 | 127.6 | 398.1 | 614.3 |
| `VectorBinaryLevels` | 14.39M req/s, 69.5 ns/req | 15.44M req/s, 64.8 ns/req | 0 | 127.6 | 395.2 | 603.8 |
| `MapLevels` | 13.67M req/s, 73.2 ns/req | 15.13M req/s, 66.1 ns/req | 109,585 | 127.6 | 404.8 | 664.8 |

Latency percentiles are per request (new order, cancel or replace, including every fill it causes) in ns, timer overhead included. This book is small (about 7,000 live orders over a few hundred levels) and stays in cache, so the containers differ less than on the ITCH data. The ladder's direct indexing is 43% faster than the vector at the median; it is the better choice when the venue enforces a price band around the touch, which a real matching engine does. `VectorLevels` stays the default because it needs no band configuration and has no bad case for a sparse book.

### Reproduce

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
# data: the first ~1.6 GB of the day; bench_itch itself trims to the last whole message
curl -s https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz | gunzip | head -c 1600000000 > prefix.itch
./build/bench_memlat --cpu 1
./build/bench_itch prefix.itch --repeat 5 --cpu 1
python3 reference/gen_match_events.py --n 2000000 --seed 1 --out events.txt
./build/bench_matching events.txt --repeat 25 --cpu 1
```

## Running on a full Nasdaq day

Without storing the day (about 10 minutes here, limited by the Python reference, which the script runs alongside):

```
tools/full_day.sh build ../data/fullday     # replay + Python reference, 15 traced symbols
```

Replay only, with a top-of-book CSV for chosen symbols (the same command without `--csv` took 119 s for the whole day here, 2.25M msg/s, limited by `curl` and `gunzip` rather than by the book):

```
curl -s https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz | gunzip \
  | ./build/replay --symbols AAPL,MSFT,SPY --times 09:45:00,12:00:00,15:59:59 \
                   --csv aapl.csv --csv-symbols AAPL --audit-every 50000000 -
```

`replay --help` lists the options (level container, trace output for the differential test, checkpoint interval, message limit). The summary prints message counts per type, system events, live and peak orders, every invariant counter, per-symbol printed volume split by execution type with the opening and closing cross prices and the regular-session high and low, and top of book at the requested times. The exit status is 1 if any anomaly or audit failure was seen.

## Data and CI

The repository contains no Nasdaq data. The sample files are published by Nasdaq for evaluation, and no terms were found that allow redistributing them, so CI (`.github/workflows/ci.yml`) uses `tests/data/synthetic.itch`, written by `reference/gen_synthetic_itch.py` with its own encoder: every message type in the specification, a consistent order flow (executions, cancels and replaces only reference live orders), deterministic for a seed (Python 3.10 to 3.13 produce identical bytes). CI builds gcc and clang in Release and in sanitizer configurations, runs `ctest`, runs the 1,000,000-event matching differential test for both flow profiles in Release, checks that the synthetic file regenerates byte for byte, and runs the mutation check.

## Limitations

* Benchmarks come from a shared 2 vCPU VM without hardware performance counters; expect different absolute numbers on bare metal. Relative comparisons were run back to back in one process.
* The all-symbol differential test covers the first 51.35M messages of the day; the full day is compared for 15 symbols. The full-day invariants cover every symbol.
* Both references were written for this project, independently of the C++ but from the same reading of the specification. A misreading shared by both would not be caught by the differential tests; the external checks above guard against that only partly.
* The ITCH book does not store MPID attribution and does not model non-displayed liquidity (Trade `P` messages are counted for volume only; the specification says they do not affect the book). Broken trades (`B`) are counted but not subtracted from volume; none occurred that day.

## License

MIT, see `LICENSE`.
