// match_replay: run the matching engine over a text event file and print
// every engine event, with periodic full book dumps. The output format is the
// one produced by reference/match_ref.py, so the two can be compared byte for
// byte.
//
//   match_replay [--levels vector|binary|map|ladder] [--checkpoint-every N]
//                [--audit-every N] [--min-price P] [--max-price P] [FILE|-]
//
// Input lines:
//   N <id> <B|S> <L|M> <D|I> <price> <qty>    new order (Limit/Market, Day/IOC)
//   X <id>                                    cancel
//   R <id> <price> <qty>                      cancel/replace
// Output lines:
//   T <taker> <maker> <price> <qty>           trade
//   L <B|S> <price> <qty> <count>             level after a change
//   A <id> <B|S> <price> <qty>                order rests
//   M <id> <price> <qty>                      amended in place
//   C <id> <qty> <U|I|M>                      cancelled: user, IOC rest, market rest
//   J <id> <D|U|Q|P|R>                        rejected: duplicate, unknown, qty, price, reserved id
//   K <event index>                           checkpoint, then one line per level:
//   B|S <price> <qty> <count> id:qty ...      best first
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "itchbook/matching.hpp"

using namespace itchbook;

namespace {

struct Printer {
    std::FILE* out;
    void on_trade(const TradeEvent& t) {
        std::fprintf(out, "T %" PRIu64 " %" PRIu64 " %u %u\n", t.taker_id, t.maker_id, t.price, t.qty);
    }
    void on_level(Side s, std::uint32_t px, std::uint64_t qty, std::uint32_t n) {
        std::fprintf(out, "L %c %u %" PRIu64 " %u\n", s == Side::buy ? 'B' : 'S', px, qty, n);
    }
    void on_rest(std::uint64_t id, Side s, std::uint32_t px, std::uint32_t qty) {
        std::fprintf(out, "A %" PRIu64 " %c %u %u\n", id, s == Side::buy ? 'B' : 'S', px, qty);
    }
    void on_amend(std::uint64_t id, std::uint32_t px, std::uint32_t qty) {
        std::fprintf(out, "M %" PRIu64 " %u %u\n", id, px, qty);
    }
    void on_cancel(std::uint64_t id, std::uint32_t qty, CancelReason r) {
        const char c = r == CancelReason::user ? 'U' : r == CancelReason::ioc_remainder ? 'I' : 'M';
        std::fprintf(out, "C %" PRIu64 " %u %c\n", id, qty, c);
    }
    void on_reject(std::uint64_t id, RejectReason r) {
        const char c = r == RejectReason::duplicate_id ? 'D'
                       : r == RejectReason::unknown_id ? 'U'
                       : r == RejectReason::bad_quantity ? 'Q'
                       : r == RejectReason::bad_price    ? 'P' : 'R';
        std::fprintf(out, "J %" PRIu64 " %c\n", id, c);
    }
};

struct Opts {
    std::string levels = "vector";
    std::string input = "-";
    std::uint64_t checkpoint_every = 0;
    std::uint64_t audit_every = 0;
    std::uint32_t min_price = 1;
    std::uint32_t max_price = 1u << 20;
};

template <template <bool> class LevelsT>
int run(const Opts& o) {
    std::FILE* in = o.input == "-" ? stdin : std::fopen(o.input.c_str(), "r");
    if (!in) {
        std::perror("open input");
        return 2;
    }
    static char obuf[1 << 20];
    std::setvbuf(stdout, obuf, _IOFBF, sizeof obuf);
    Printer p{stdout};
    EngineConfig cfg;
    cfg.min_price = o.min_price;
    cfg.max_price = o.max_price;
    MatchingEngine<Printer, LevelsT> eng(p, cfg);

    auto dump = [&](std::uint64_t idx) {
        std::fprintf(stdout, "K %" PRIu64 "\n", idx);
        for (Side s : {Side::buy, Side::sell}) {
            eng.for_each_level(s, [&](const Level& l) {
                std::fprintf(stdout, "%c %u %" PRIu64 " %u", s == Side::buy ? 'B' : 'S', l.price, l.shares, l.count);
                eng.for_each_order(l, [&](const MatchOrder& m) {
                    std::fprintf(stdout, " %" PRIu64 ":%u", m.id, m.qty);
                });
                std::fputc('\n', stdout);
                return true;
            });
        }
    };

    char line[256];
    std::uint64_t idx = 0;
    int status = 0;
    while (std::fgets(line, sizeof line, in)) {
        char kind = 0, side = 0, type = 0, tif = 0;
        unsigned long long id = 0, price = 0, qty = 0;
        if (line[0] == 'N' && std::sscanf(line, "%c %llu %c %c %c %llu %llu", &kind, &id, &side, &type, &tif, &price,
                                          &qty) == 7) {
            eng.submit(NewOrder{id, side == 'B' ? Side::buy : Side::sell,
                                type == 'M' ? OrderType::market : OrderType::limit,
                                tif == 'I' ? TimeInForce::ioc : TimeInForce::day, static_cast<std::uint32_t>(price),
                                static_cast<std::uint32_t>(qty)});
        } else if (line[0] == 'X' && std::sscanf(line, "%c %llu", &kind, &id) == 2) {
            eng.cancel(id);
        } else if (line[0] == 'R' && std::sscanf(line, "%c %llu %llu %llu", &kind, &id, &price, &qty) == 4) {
            eng.replace(id, static_cast<std::uint32_t>(price), static_cast<std::uint32_t>(qty));
        } else {
            std::fprintf(stderr, "bad input line %" PRIu64 ": %s", idx, line);
            return 2;
        }
        ++idx;
        if (o.checkpoint_every && idx % o.checkpoint_every == 0) dump(idx);
        if (o.audit_every && idx % o.audit_every == 0) {
            const std::string a = eng.audit();
            if (!a.empty()) {
                std::fprintf(stderr, "audit failed after event %" PRIu64 ": %s\n", idx, a.c_str());
                status = 1;
            }
        }
    }
    if (o.checkpoint_every) dump(idx);
    const std::string a = eng.audit();
    if (!a.empty()) {
        std::fprintf(stderr, "final audit failed: %s\n", a.c_str());
        status = 1;
    }
    std::fflush(stdout);
    std::fprintf(stderr, "match_replay: %" PRIu64 " events, %zu live orders, rehashes %zu, pool grows %zu\n", idx,
                 eng.live_orders(), eng.map_rehashes(), eng.pool_grows());
    if (in != stdin) std::fclose(in);
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) std::exit(2);
            return argv[++i];
        };
        if (a == "--levels") o.levels = next();
        else if (a == "--checkpoint-every") o.checkpoint_every = std::strtoull(next(), nullptr, 10);
        else if (a == "--audit-every") o.audit_every = std::strtoull(next(), nullptr, 10);
        else if (a == "--min-price") o.min_price = static_cast<std::uint32_t>(std::strtoul(next(), nullptr, 10));
        else if (a == "--max-price") o.max_price = static_cast<std::uint32_t>(std::strtoul(next(), nullptr, 10));
        else o.input = a;
    }
    if (o.levels == "vector") return run<VectorLevels>(o);
    if (o.levels == "binary") return run<VectorBinaryLevels>(o);
    if (o.levels == "map") return run<MapLevels>(o);
    if (o.levels == "ladder") return run<LadderLevels>(o);
    std::fprintf(stderr, "unknown --levels %s\n", o.levels.c_str());
    return 2;
}
