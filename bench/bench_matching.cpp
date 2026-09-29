// bench_matching: matching engine cost per request on synthetic order flow.
//
//   bench_matching EVENTS_FILE [--levels vector,binary,map,ladder] [--cpu N] [--repeat R]
//
// EVENTS_FILE is produced by reference/gen_match_events.py (the same flow the
// differential test uses). Events are parsed into memory first. For each
// container: one cold pass, R warm throughput passes (engine cleared in
// between, capacity kept) timed with steady_clock, and one latency pass timing
// every request with lfence/rdtsc ... rdtscp/lfence.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "itchbook/matching.hpp"

using namespace itchbook;

namespace {

struct Event {
    char kind;  // 'N', 'X', 'R'
    NewOrder o;
};

// Keeps a running summary so the compiler cannot discard the engine's work.
struct Sink {
    std::uint64_t trades = 0, traded_qty = 0, levels = 0, rests = 0, cancels = 0, rejects = 0;
    void on_trade(const TradeEvent& t) {
        ++trades;
        traded_qty += t.qty;
    }
    void on_level(Side, std::uint32_t, std::uint64_t q, std::uint32_t) { levels += q; }
    void on_rest(std::uint64_t, Side, std::uint32_t, std::uint32_t) { ++rests; }
    void on_cancel(std::uint64_t, std::uint32_t, CancelReason) { ++cancels; }
    void on_reject(std::uint64_t, RejectReason) { ++rejects; }
};

std::vector<Event> load(const char* path) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        std::perror("open");
        std::exit(2);
    }
    std::vector<Event> ev;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        char k = 0, side = 0, type = 0, tif = 0;
        unsigned long long id = 0, px = 0, q = 0;
        Event e{};
        if (line[0] == 'N' && std::sscanf(line, "%c %llu %c %c %c %llu %llu", &k, &id, &side, &type, &tif, &px, &q) == 7) {
            e = Event{'N', NewOrder{id, side == 'B' ? Side::buy : Side::sell,
                                    type == 'M' ? OrderType::market : OrderType::limit,
                                    tif == 'I' ? TimeInForce::ioc : TimeInForce::day, static_cast<std::uint32_t>(px),
                                    static_cast<std::uint32_t>(q)}};
        } else if (line[0] == 'X' && std::sscanf(line, "%c %llu", &k, &id) == 2) {
            e.kind = 'X';
            e.o.id = id;
        } else if (line[0] == 'R' && std::sscanf(line, "%c %llu %llu %llu", &k, &id, &px, &q) == 4) {
            e.kind = 'R';
            e.o.id = id;
            e.o.price = static_cast<std::uint32_t>(px);
            e.o.qty = static_cast<std::uint32_t>(q);
        } else {
            continue;
        }
        ev.push_back(e);
    }
    std::fclose(f);
    return ev;
}

template <class E>
[[gnu::always_inline]] inline void apply(E& eng, const Event& e) {
    switch (e.kind) {
        case 'N': eng.submit(e.o); break;
        case 'X': eng.cancel(e.o.id); break;
        default: eng.replace(e.o.id, e.o.price, e.o.qty); break;
    }
}

template <template <bool> class L>
void run(const char* name, const std::vector<Event>& ev, int repeat, double tpn, std::uint64_t overhead) {
    Sink sink;
    EngineConfig cfg;
    cfg.expected_orders = std::size_t{1} << 20;
    cfg.expected_levels = std::size_t{1} << 16;
    auto eng = std::make_unique<MatchingEngine<Sink, L>>(sink, cfg);
    const double n = static_cast<double>(ev.size());
    std::printf("\n=== level container: %s ===\n", name);

    auto t0 = std::chrono::steady_clock::now();
    for (const auto& e : ev) apply(*eng, e);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const Sink ref = sink;
    std::printf("cold pass            %8.3f s  %7.2f M req/s  %6.1f ns/req  (trades %llu, live orders at end %zu)\n", s,
                n / s / 1e6, s * 1e9 / n, static_cast<unsigned long long>(ref.trades), eng->live_orders());

    std::vector<double> secs;
    secs.reserve(static_cast<std::size_t>(repeat));
    const std::uint64_t a0 = bench::g_allocs;
    for (int r = 0; r < repeat; ++r) {
        eng->clear();
        sink = Sink{};
        t0 = std::chrono::steady_clock::now();
        for (const auto& e : ev) apply(*eng, e);
        secs.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        if (sink.trades != ref.trades || sink.traded_qty != ref.traded_qty) {
            std::printf("ERROR: pass %d differs from the cold pass\n", r);
            std::exit(1);
        }
    }
    const std::uint64_t allocs = bench::g_allocs - a0;
    std::sort(secs.begin(), secs.end());
    std::printf("warm passes x%-2d best %8.3f s  %7.2f M req/s  %6.1f ns/req   median %.3f s %7.2f M req/s\n", repeat,
                secs.front(), n / secs.front() / 1e6, secs.front() * 1e9 / n, secs[secs.size() / 2],
                n / secs[secs.size() / 2] / 1e6);
    std::printf("allocations during warm passes: %llu\n", static_cast<unsigned long long>(allocs));

    eng->clear();
    sink = Sink{};
    std::vector<std::uint32_t> samples(ev.size());
    std::vector<std::uint32_t> s_new_rest, s_new_trade, s_cancel, s_replace;
    const std::uint64_t a1 = bench::g_allocs;
    for (std::size_t i = 0; i < ev.size(); ++i) {
        const std::uint64_t a = bench::tsc_start();
        apply(*eng, ev[i]);
        const std::uint64_t b = bench::tsc_stop();
        samples[i] = static_cast<std::uint32_t>(b - a);
    }
    const std::uint64_t allocs_lat = bench::g_allocs - a1;
    bench::do_not_optimize(sink);
    // Split by request kind (and, for new orders, whether it traded) in a
    // second, untimed replay so the timed loop stays minimal.
    {
        Sink s2;
        MatchingEngine<Sink, L> e2(s2, cfg);
        for (std::size_t i = 0; i < ev.size(); ++i) {
            const std::uint64_t before = s2.trades;
            apply(e2, ev[i]);
            if (ev[i].kind == 'N') (s2.trades != before ? s_new_trade : s_new_rest).push_back(samples[i]);
            else if (ev[i].kind == 'X') s_cancel.push_back(samples[i]);
            else s_replace.push_back(samples[i]);
        }
    }
    std::printf("latency pass: allocations %llu, timer overhead %.1f ns included below\n",
                static_cast<unsigned long long>(allocs_lat), static_cast<double>(overhead) / tpn);
    bench::print_header();
    bench::print_row("all requests", bench::percentiles(samples, tpn));
    bench::print_row("new, no trade", bench::percentiles(s_new_rest, tpn));
    bench::print_row("new, traded", bench::percentiles(s_new_trade, tpn));
    bench::print_row("cancel", bench::percentiles(s_cancel, tpn));
    bench::print_row("replace", bench::percentiles(s_replace, tpn));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bench_matching EVENTS_FILE [--levels vector,binary,map,ladder] [--cpu N] [--repeat R]\n");
        return 2;
    }
    const char* path = nullptr;
    std::string levels = "vector,binary,map,ladder";
    int cpu = 1, repeat = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--levels" && i + 1 < argc) levels = argv[++i];
        else if (a == "--cpu" && i + 1 < argc) cpu = std::atoi(argv[++i]);
        else if (a == "--repeat" && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else path = argv[i];
    }
    const bool pinned = bench::pin_to_cpu(cpu);
    std::printf("cpu model        %s\n", bench::cpu_model().c_str());
    std::printf("load average     %s (at start)\n", bench::loadavg().c_str());
    std::printf("hardware threads %u (pinned to cpu %d: %s)\n", std::thread::hardware_concurrency(), cpu,
                pinned ? "yes" : "FAILED");
    const double tpn = bench::calibrate_tsc();
    const std::uint64_t overhead = bench::timer_overhead();
    std::printf("tsc              %.4f ticks/ns (calibrated against steady_clock over 500 ms)\n", tpn);
    const std::vector<Event> ev = load(path);
    std::printf("input            %s: %zu requests\n", path, ev.size());

    std::size_t start = 0;
    while (start <= levels.size()) {
        const std::size_t c = levels.find(',', start);
        const std::string l = levels.substr(start, c == std::string::npos ? std::string::npos : c - start);
        if (l == "vector") run<VectorLevels>("vector (sorted, linear scan from best, then binary)", ev, repeat, tpn, overhead);
        else if (l == "binary") run<VectorBinaryLevels>("vector (sorted, binary search only)", ev, repeat, tpn, overhead);
        else if (l == "map") run<MapLevels>("std::map", ev, repeat, tpn, overhead);
        else if (l == "ladder") run<LadderLevels>("ladder (dense array + bitmap)", ev, repeat, tpn, overhead);
        else if (!l.empty()) std::printf("unknown container %s\n", l.c_str());
        if (c == std::string::npos) break;
        start = c + 1;
    }
    std::printf("\nload average     %s (at end)\n", bench::loadavg().c_str());
    return 0;
}
