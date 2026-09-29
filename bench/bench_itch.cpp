// bench_itch: parse + order book update cost on real ITCH data held in memory.
//
//   bench_itch FILE [--levels vector,binary,map,vector-identity] [--cpu N] [--repeat R]
//              [--max-bytes B]
//
// For each price level container:
//   1. cold pass over the whole buffer (first touch of all book memory);
//   2. R throughput passes (books cleared in between, capacity kept), timed
//      with steady_clock over the whole buffer, no per-message timer;
//   3. one latency pass that times every message individually with
//      lfence/rdtsc ... rdtscp/lfence, TSC calibrated against steady_clock.
// Allocations made through operator new are counted during steps 2 and 3.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "bench_common.hpp"
#include "itchbook/book.hpp"
#include "itchbook/feed.hpp"
#include "itchbook/itch.hpp"

using namespace itchbook;

namespace {

std::vector<char> load(const std::string& path, std::size_t max_bytes) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::perror("open");
        std::exit(2);
    }
    struct stat st{};
    ::fstat(fd, &st);
    std::size_t n = static_cast<std::size_t>(st.st_size);
    if (max_bytes && max_bytes < n) n = max_bytes;
    std::vector<char> buf(n);
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::read(fd, buf.data() + got, n - got);
        if (r <= 0) break;
        got += static_cast<std::size_t>(r);
    }
    ::close(fd);
    buf.resize(got);
    // Trim to the last complete message.
    std::size_t off = 0;
    while (off + 2 <= buf.size()) {
        const std::size_t len = itch::be16(buf.data() + off);
        if (off + 2 + len > buf.size()) break;
        off += 2 + len;
    }
    buf.resize(off);
    return buf;
}

std::uint64_t count_messages(const std::vector<char>& buf) {
    std::uint64_t n = 0;
    for (std::size_t off = 0; off + 2 <= buf.size(); off += 2 + itch::be16(buf.data() + off)) ++n;
    return n;
}

template <template <bool> class L, MapHash Hash = MapHash::fibonacci>
void run(const char* name, const std::vector<char>& buf, std::uint64_t n_msgs, int repeat, double tpn,
         std::uint64_t overhead) {
    using Books = ItchBooks<L, Hash>;
    auto books = std::make_unique<Books>(std::size_t{1} << 22, std::size_t{1} << 21);
    BookHandler<Books> h(*books);
    const double msgs = static_cast<double>(n_msgs);

    std::printf("\n=== level container: %s ===\n", name);

    // 1. cold pass
    auto t0 = std::chrono::steady_clock::now();
    itch::dispatch_buffer(buf.data(), buf.size(), h);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const std::size_t live_after = books->live_orders();
    const std::size_t peak = books->peak_orders();
    std::printf("cold pass            %8.3f s  %7.2f M msg/s  %6.1f ns/msg  (live orders at end %zu, peak %zu)\n", s,
                msgs / s / 1e6, s * 1e9 / msgs, live_after, peak);

    // 2. throughput passes
    std::vector<double> secs;
    secs.reserve(static_cast<std::size_t>(repeat));
    const std::uint64_t allocs0 = bench::g_allocs;
    for (int r = 0; r < repeat; ++r) {
        books->clear();
        t0 = std::chrono::steady_clock::now();
        itch::dispatch_buffer(buf.data(), buf.size(), h);
        secs.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        if (books->live_orders() != live_after) {
            std::printf("ERROR: pass %d ended with %zu live orders\n", r, books->live_orders());
            std::exit(1);
        }
    }
    const std::uint64_t allocs_tp = bench::g_allocs - allocs0;
    std::sort(secs.begin(), secs.end());
    const double best = secs.front(), med = secs[secs.size() / 2];
    std::printf("warm passes x%-2d best %8.3f s  %7.2f M msg/s  %6.1f ns/msg   median %.3f s %7.2f M msg/s\n",
                repeat, best, msgs / best / 1e6, best * 1e9 / msgs, med, msgs / med / 1e6);
    std::printf("allocations during warm passes: %llu (%.3f per pass per 1M messages)\n",
                static_cast<unsigned long long>(allocs_tp),
                static_cast<double>(allocs_tp) / repeat / (msgs / 1e6));

    // 2b. throughput with lookahead prefetching
    auto lookahead = [&](auto step_tag) {
        constexpr std::size_t Step = decltype(step_tag)::value;
        std::vector<double> ls;
        ls.reserve(static_cast<std::size_t>(repeat));
        const std::uint64_t la0 = bench::g_allocs;
        for (int r = 0; r < repeat; ++r) {
            books->clear();
            const auto l0 = std::chrono::steady_clock::now();
            dispatch_buffer_lookahead<Step>(buf.data(), buf.size(), h, *books);
            ls.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - l0).count());
            if (books->live_orders() != live_after) {
                std::printf("ERROR: lookahead pass ended with %zu live orders\n", books->live_orders());
                std::exit(1);
            }
        }
        std::sort(ls.begin(), ls.end());
        std::printf("lookahead %zu x%-2d  best %8.3f s  %7.2f M msg/s  %6.1f ns/msg   median %.3f s %7.2f M msg/s  (allocs %llu)\n",
                    Step, repeat, ls.front(), msgs / ls.front() / 1e6, ls.front() * 1e9 / msgs, ls[ls.size() / 2],
                    msgs / ls[ls.size() / 2] / 1e6, static_cast<unsigned long long>(bench::g_allocs - la0));
    };
    lookahead(std::integral_constant<std::size_t, 2>{});
    lookahead(std::integral_constant<std::size_t, 4>{});
    lookahead(std::integral_constant<std::size_t, 8>{});

    // 3. latency pass
    books->clear();
    std::vector<std::uint32_t> samples(n_msgs);
    std::vector<char> types(n_msgs);
    const std::uint64_t allocs1 = bench::g_allocs;
    std::size_t i = 0;
    const char* p = buf.data();
    const char* end = p + buf.size();
    while (p + 2 <= end) {
        const std::size_t len = itch::be16(p);
        const char* m = p + 2;
        const std::uint64_t a = bench::tsc_start();
        itch::dispatch(m, len, h);
        const std::uint64_t b = bench::tsc_stop();
        samples[i] = static_cast<std::uint32_t>(b - a);
        types[i] = m[0];
        ++i;
        p = m + len;
    }
    const std::uint64_t allocs_lat = bench::g_allocs - allocs1;
    bench::do_not_optimize(h.results);
    if (h.anomalies() != 0) {
        std::printf("ERROR: %llu book anomalies\n", static_cast<unsigned long long>(h.anomalies()));
        std::exit(1);
    }
    std::printf("latency pass: %zu messages, allocations %llu, timer overhead %.1f ns (%llu ticks) included below\n",
                i, static_cast<unsigned long long>(allocs_lat), static_cast<double>(overhead) / tpn,
                static_cast<unsigned long long>(overhead));
    std::printf("per-message latency, ns (parse + book update, timer overhead included):\n");
    bench::print_header();
    {
        std::vector<std::uint32_t> all(samples);
        bench::print_row("all messages", bench::percentiles(all, tpn));
    }
    const std::pair<char, const char*> kinds[] = {{'A', "A add"},         {'F', "F add mpid"},
                                                  {'D', "D delete"},      {'U', "U replace"},
                                                  {'E', "E executed"},    {'X', "X cancel"},
                                                  {'C', "C exec w/ price"}, {0, "non-book types"}};
    for (const auto& [t, label] : kinds) {
        std::vector<std::uint32_t> sel;
        for (std::size_t k = 0; k < i; ++k) {
            const char ty = types[k];
            const bool book = ty == 'A' || ty == 'F' || ty == 'D' || ty == 'U' || ty == 'E' || ty == 'X' || ty == 'C';
            if ((t != 0 && ty == t) || (t == 0 && !book)) sel.push_back(samples[k]);
        }
        bench::print_row(label, bench::percentiles(sel, tpn));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bench_itch FILE [--levels vector,binary,map,vector-identity] [--cpu N] [--repeat R] [--max-bytes B]\n");
        return 2;
    }
    std::string path, levels = "vector,binary,map,vector-identity";
    int cpu = 1, repeat = 5;
    std::size_t max_bytes = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--levels" && i + 1 < argc) levels = argv[++i];
        else if (a == "--cpu" && i + 1 < argc) cpu = std::atoi(argv[++i]);
        else if (a == "--repeat" && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else if (a == "--max-bytes" && i + 1 < argc) max_bytes = std::strtoull(argv[++i], nullptr, 10);
        else path = a;
    }
    const bool pinned = bench::pin_to_cpu(cpu);
    std::printf("cpu model        %s\n", bench::cpu_model().c_str());
    std::printf("load average     %s (at start)\n", bench::loadavg().c_str());
    std::printf("hardware threads %u (pinned to cpu %d: %s)\n", std::thread::hardware_concurrency(), cpu,
                pinned ? "yes" : "FAILED");
    const double tpn = bench::calibrate_tsc();
    const std::uint64_t overhead = bench::timer_overhead();
    std::printf("tsc              %.4f ticks/ns (calibrated against steady_clock over 500 ms)\n", tpn);

    const std::vector<char> buf = load(path, max_bytes);
    const std::uint64_t n = count_messages(buf);
    std::printf("input            %s: %zu bytes, %llu messages, held in memory\n", path.c_str(), buf.size(),
                static_cast<unsigned long long>(n));

    std::size_t start = 0;
    while (start <= levels.size()) {
        const std::size_t c = levels.find(',', start);
        const std::string l = levels.substr(start, c == std::string::npos ? std::string::npos : c - start);
        if (l == "vector") run<VectorLevels>("vector (sorted, linear scan from best, then binary)", buf, n, repeat, tpn, overhead);
        else if (l == "binary") run<VectorBinaryLevels>("vector (sorted, binary search only)", buf, n, repeat, tpn, overhead);
        else if (l == "map") run<MapLevels>("std::map", buf, n, repeat, tpn, overhead);
        else if (l == "vector-identity")
            run<VectorLevels, MapHash::identity>("vector, order map with identity hashing", buf, n, repeat, tpn, overhead);
        else if (!l.empty()) std::printf("unknown container %s\n", l.c_str());
        if (c == std::string::npos) break;
        start = c + 1;
    }
    std::printf("\nload average     %s (at end)\n", bench::loadavg().c_str());
    return 0;
}
