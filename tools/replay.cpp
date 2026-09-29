// replay: rebuild every Nasdaq order book from a raw TotalView-ITCH 5.0 stream
// and report message counts, invariant checks, volumes and top of book.
//
//   replay [options] [FILE|-]          (default: stdin)
//
//   --symbols A,B,C        symbols for the volume / top-of-book report
//                          (default AAPL,MSFT,SPY,QQQ,AMZN,TSLA)
//   --times HH:MM:SS,...   report top of book as of these times
//                          (default 09:35:00,12:00:00,15:59:59)
//   --csv FILE             write top of book to FILE whenever it changes
//   --csv-symbols A,B      symbols for --csv (default: --symbols)
//   --trace FILE           differential test trace (see reference/itch_ref.py)
//   --trace-symbols A,B|ALL
//   --checkpoint-every N   full depth dump in the trace every N messages
//   --audit-every N        full structural audit of all books every N messages
//                          (default 0 = only at end of stream)
//   --max-messages N       stop after N messages
//   --levels vector|binary|map   price level container (default vector)
//
// All invariant counters are printed in the summary; the exit status is 1 if
// any anomaly was seen or an audit failed.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "itchbook/book.hpp"
#include "itchbook/feed.hpp"
#include "itchbook/itch.hpp"

using namespace itchbook;

namespace {

struct Options {
    std::string input = "-";
    std::vector<std::string> symbols{"AAPL", "MSFT", "SPY", "QQQ", "AMZN", "TSLA"};
    std::vector<std::uint64_t> times;
    std::string csv;
    std::vector<std::string> csv_symbols;
    std::string trace;
    std::vector<std::string> trace_symbols;
    bool trace_all = false;
    std::uint64_t checkpoint_every = 0;
    std::uint64_t audit_every = 0;
    std::uint64_t max_messages = ~std::uint64_t{0};
    std::string levels = "vector";
};

std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t c = s.find(',', start);
        const std::size_t end = c == std::string_view::npos ? s.size() : c;
        if (end > start) out.emplace_back(s.substr(start, end - start));
        if (c == std::string_view::npos) break;
        start = c + 1;
    }
    return out;
}

std::uint64_t parse_time(const std::string& t) {
    unsigned h = 0, m = 0;
    double sec = 0;
    if (std::sscanf(t.c_str(), "%u:%u:%lf", &h, &m, &sec) != 3) {
        std::fprintf(stderr, "bad time '%s' (want HH:MM:SS)\n", t.c_str());
        std::exit(2);
    }
    return (std::uint64_t{h} * 3600 + m * 60) * 1'000'000'000ull + static_cast<std::uint64_t>(sec * 1e9 + 0.5);
}

std::string fmt_time(std::uint64_t ns) {
    char b[32];
    const std::uint64_t giga = 1'000'000'000;
    const std::uint64_t s = ns / giga;
    const std::uint64_t frac = ns % giga;
    std::snprintf(b, sizeof b, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%09" PRIu64, s / 3600, s / 60 % 60, s % 60,
                  frac);
    return b;
}

std::string fmt_px(std::uint64_t px4) {
    char b[32];
    std::snprintf(b, sizeof b, "%" PRIu64 ".%04" PRIu64, px4 / 10000, px4 % 10000);
    return b;
}

// Per-locate trading statistics.
struct SymbolStats {
    std::uint64_t exec_e = 0;          // 'E' shares (always printable)
    std::uint64_t exec_c_print = 0;    // 'C' shares, printable
    std::uint64_t exec_c_noprint = 0;  // 'C' shares, non-printable (counted in a cross print)
    std::uint64_t trade_p = 0;         // 'P' shares (non-displayed executions)
    std::uint64_t cross_open = 0, cross_close = 0, cross_other = 0;
    std::uint32_t open_cross_px = 0, close_cross_px = 0;
    std::uint64_t notional_e = 0;      // sum shares*price(4) for 'E' at the order's price
    // Highest and lowest printable execution price in [09:30:00, 16:00:01),
    // which includes the opening and closing crosses: over all prints, and
    // over prints of at least one round lot (in 2019 odd-lot trades did not
    // update the consolidated high and low, so only the second pair is
    // comparable with published daily bars).
    std::uint32_t round_lot = 100;
    std::uint32_t reg_hi = 0, reg_lo = 0, rl_hi = 0, rl_lo = 0;
    void print(std::uint64_t ts, std::uint32_t px, std::uint64_t shares) {
        if (ts < kRegStart || ts >= kRegEnd) return;
        if (px > reg_hi) reg_hi = px;
        if (reg_lo == 0 || px < reg_lo) reg_lo = px;
        if (shares < round_lot) return;
        if (px > rl_hi) rl_hi = px;
        if (rl_lo == 0 || px < rl_lo) rl_lo = px;
    }
    static constexpr std::uint64_t kRegStart = 34200ull * 1'000'000'000ull;  // 09:30:00
    static constexpr std::uint64_t kRegEnd = 57601ull * 1'000'000'000ull;    // 16:00:01
};

template <template <bool> class LevelsT>
class Replayer {
public:
    using Books = ItchBooks<LevelsT>;

    explicit Replayer(const Options& opt) : opt_(opt), books_(std::make_unique<Books>(std::size_t{1} << 23)) {
        for (const auto& s : opt_.symbols) want_report_[s] = true;
        for (const auto& s : (opt_.csv_symbols.empty() ? opt_.symbols : opt_.csv_symbols)) want_csv_[s] = true;
        for (const auto& s : opt_.trace_symbols) want_trace_[s] = true;
        if (!opt_.csv.empty()) {
            csv_ = std::fopen(opt_.csv.c_str(), "w");
            if (!csv_) die("cannot open csv");
            std::fprintf(csv_, "timestamp_ns,time,symbol,bid_px,bid_shares,bid_orders,ask_px,ask_shares,ask_orders\n");
        }
        if (!opt_.trace.empty()) {
            trace_ = opt_.trace == "-" ? stdout : std::fopen(opt_.trace.c_str(), "w");
            if (!trace_) die("cannot open trace");
            static char tbuf[1 << 20];
            std::setvbuf(trace_, tbuf, _IOFBF, sizeof tbuf);
        }
    }
    ~Replayer() {
        if (csv_) std::fclose(csv_);
        if (trace_ && trace_ != stdout) std::fclose(trace_);
    }

    int run() {
        int fd = 0;
        if (opt_.input != "-") {
            fd = ::open(opt_.input.c_str(), O_RDONLY);
            if (fd < 0) die("cannot open input");
        }
        const auto t0 = std::chrono::steady_clock::now();
        const StreamStats st = read_stream(fd, *this, [this] { return msg_index_ >= opt_.max_messages; });
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (fd != 0) ::close(fd);
        input_ended_ = true;
        while (next_time_ < opt_.times.size()) snapshot_times(~std::uint64_t{0});
        if (trace_) {
            if (opt_.checkpoint_every) checkpoint();
            std::fflush(trace_);
        }
        const std::string final_audit = books_->audit();
        ++audits_;
        if (!final_audit.empty()) audit_failures_.push_back("end: " + final_audit);
        return report(st, secs);
    }

    // ------------------------------------------------------------ handler

    template <class M>
    void on(const M& m) {
        const std::uint64_t idx = msg_index_;
        if (idx >= opt_.max_messages) return;  // buffer may hold more than asked for
        ++msg_index_;
        ++type_counts_[static_cast<unsigned char>(M::kType)];
        const std::uint64_t ts = m.timestamp();
        if (ts < last_ts_) ++ts_regressions_;
        last_ts_ = ts;
        if (next_time_ < opt_.times.size() && ts >= opt_.times[next_time_]) snapshot_times(ts);
        handle(m, idx);
        if (opt_.audit_every && (idx + 1) % opt_.audit_every == 0) {
            const std::string a = books_->audit();
            ++audits_;
            if (!a.empty() && audit_failures_.size() < 10) audit_failures_.push_back("msg " + std::to_string(idx) + ": " + a);
        }
        if (trace_ && opt_.checkpoint_every && (idx + 1) % opt_.checkpoint_every == 0) checkpoint();
    }
    void on_bad_length(const char*, std::size_t) {
        ++bad_length_;
        ++msg_index_;
    }
    void on_unknown(const char* p, std::size_t) {
        ++unknown_type_;
        ++msg_index_;
        ++type_counts_[static_cast<unsigned char>(p[0])];
    }

private:
    [[noreturn]] static void die(const char* what) {
        std::perror(what);
        std::exit(2);
    }

    // Non-book messages.
    template <class M>
    void handle(const M&, std::uint64_t) {}

    void handle(const itch::SystemEvent& m, std::uint64_t) {
        system_events_.emplace_back(m.event_code(), m.timestamp());
    }
    void handle(const itch::StockDirectory& m, std::uint64_t) {
        const std::string sym(itch::trim_alpha(m.stock()));
        const std::uint16_t loc = m.stock_locate();
        if (!symbol_of_[loc].empty() && symbol_of_[loc] != sym) ++directory_conflicts_;
        symbol_of_[loc] = sym;
        raw_symbol_[loc] = std::string(m.stock());
        stats_[loc].round_lot = m.round_lot_size();
        locate_of_[sym] = loc;
        ++directory_count_;
        report_[loc] = want_report_.count(sym) > 0;
        csv_on_[loc] = csv_ && want_csv_.count(sym) > 0;
        trace_on_[loc] = trace_ && (opt_.trace_all || want_trace_.count(sym) > 0);
    }
    void handle(const itch::Trade& m, std::uint64_t) {
        stats_[m.stock_locate()].trade_p += m.shares();
        stats_[m.stock_locate()].print(m.timestamp(), m.price(), m.shares());
        if (m.order_reference_number() != 0) ++trade_nonzero_ref_;
        check_symbol(m.stock_locate(), m.stock());
    }
    void handle(const itch::CrossTrade& m, std::uint64_t) {
        SymbolStats& s = stats_[m.stock_locate()];
        if (m.shares() > 0) s.print(m.timestamp(), m.cross_price(), m.shares());
        switch (m.cross_type()) {
            case 'O': s.cross_open += m.shares(); s.open_cross_px = m.cross_price(); break;
            case 'C': s.cross_close += m.shares(); s.close_cross_px = m.cross_price(); break;
            default: s.cross_other += m.shares(); break;
        }
        check_symbol(m.stock_locate(), m.stock());
    }
    void handle(const itch::BrokenTrade&, std::uint64_t) { ++broken_trades_; }
    void handle(const itch::StockTradingAction& m, std::uint64_t) {
        trading_state_[m.stock_locate()] = m.trading_state();
        check_symbol(m.stock_locate(), m.stock());
    }

    // Book messages.
    void handle(const itch::AddOrder& m, std::uint64_t idx) {
        check_symbol(m.stock_locate(), m.stock());
        book_msg(m, idx, m.stock_locate());
    }
    void handle(const itch::AddOrderMpid& m, std::uint64_t idx) {
        check_symbol(m.stock_locate(), m.stock());
        book_msg(m, idx, m.stock_locate());
    }
    void handle(const itch::OrderExecuted& m, std::uint64_t idx) {
        if (const Order* o = books_->find(m.order_reference_number()))
            if (o->locate == m.stock_locate()) {
                stats_[o->locate].exec_e += m.executed_shares();
                stats_[o->locate].print(m.timestamp(), o->price, m.executed_shares());
                stats_[o->locate].notional_e += std::uint64_t{m.executed_shares()} * o->price;
            }
        book_msg(m, idx, m.stock_locate());
    }
    void handle(const itch::OrderExecutedWithPrice& m, std::uint64_t idx) {
        if (m.printable() == 'Y') {
            stats_[m.stock_locate()].exec_c_print += m.executed_shares();
            stats_[m.stock_locate()].print(m.timestamp(), m.execution_price(), m.executed_shares());
        } else stats_[m.stock_locate()].exec_c_noprint += m.executed_shares();
        if (m.printable() != 'Y' && m.printable() != 'N') ++bad_printable_;
        book_msg(m, idx, m.stock_locate());
    }
    void handle(const itch::OrderCancel& m, std::uint64_t idx) { book_msg(m, idx, m.stock_locate()); }
    void handle(const itch::OrderDelete& m, std::uint64_t idx) { book_msg(m, idx, m.stock_locate()); }
    void handle(const itch::OrderReplace& m, std::uint64_t idx) { book_msg(m, idx, m.stock_locate()); }

    template <class M>
    void book_msg(const M& m, std::uint64_t idx, std::uint16_t loc) {
        const TopOfBook before = (csv_on_[loc]) ? books_->top(loc) : TopOfBook{};
        const Result r = applier_.apply(m);
        ++results_[static_cast<unsigned char>(M::kType)][static_cast<std::size_t>(r)];
        if (r != Result::ok && anomaly_examples_.size() < 20)
            anomaly_examples_.push_back(std::string(1, M::kType) + " msg " + std::to_string(idx) + " -> " + to_string(r));
        if (symbol_of_[loc].empty()) ++book_msgs_unknown_locate_;
        const TopOfBook t = books_->top(loc);
        if (t.bid_price && t.ask_price && t.bid_price >= t.ask_price) [[unlikely]] {
            const std::uint64_t ts = m.timestamp();
            const int session = ts < kOpen ? 0 : ts < kClose ? 1 : 2;
            const bool crossed = t.bid_price > t.ask_price;
            ++(crossed ? crossed_after_msg_ : locked_after_msg_)[session];
            if (crossed) ++(trading_state_[loc] == 'T' ? crossed_while_trading_ : crossed_while_not_trading_);
            // A halt or IPO cross is published as the Cross Trade and the
            // Trading Action first, then one non-printable 'C' per order
            // executed in the cross; until the last one arrives the book can
            // show crossed interest although the security is trading.
            const std::uint64_t other_before = crossed_trading_other_;
            if (crossed && trading_state_[loc] == 'T') {
                if constexpr (std::is_same_v<M, itch::OrderExecutedWithPrice>) {
                    if (m.printable() == 'N') ++crossed_trading_cross_exec_;
                    else ++crossed_trading_other_;
                } else {
                    ++crossed_trading_other_;
                }
                if (crossed_trading_other_ > other_before && crossed_other_examples_.size() < 12)
                    crossed_other_examples_.push_back(fmt_time(ts) + " " + symbol_of_[loc] + " bid " +
                                                      fmt_px(t.bid_price) + " ask " + fmt_px(t.ask_price) +
                                                      " after '" + M::kType + "'");
            }
            if (crossed) crossed_locates_[session].insert(loc);
            if (crossed && trading_state_[loc] == 'T' && crossed_in_t_examples_.size() < 8)
                crossed_in_t_examples_.push_back(fmt_time(ts) + " " + symbol_of_[loc] + " bid " +
                                                 fmt_px(t.bid_price) + " ask " + fmt_px(t.ask_price) + " after '" +
                                                 M::kType + "'");
            if (crossed && session == 1 && crossed_examples_.size() < 5)
                crossed_examples_.push_back(fmt_time(ts) + " " + symbol_of_[loc] + " bid " + fmt_px(t.bid_price) +
                                            " ask " + fmt_px(t.ask_price) + " after '" + M::kType +
                                            "', trading state '" + trading_state_[loc] + "'");
        }
        if (csv_on_[loc] && !(t == before)) write_csv(m.timestamp(), loc, t);
        if (trace_on_[loc]) {
            ++trace_lines_;
            std::fprintf(trace_, "%" PRIu64 " %u %d %u %" PRIu64 " %u %u %" PRIu64 " %u\n", idx, loc,
                         static_cast<int>(r), t.bid_price, t.bid_shares, t.bid_orders, t.ask_price, t.ask_shares,
                         t.ask_orders);
        }
    }

    void check_symbol(std::uint16_t loc, std::string_view raw) {
        if (raw_symbol_[loc].empty()) return;
        if (raw != raw_symbol_[loc]) ++symbol_mismatches_;
    }

    void write_csv(std::uint64_t ts, std::uint16_t loc, const TopOfBook& t) {
        std::fprintf(csv_, "%" PRIu64 ",%s,%s,%s,%" PRIu64 ",%u,%s,%" PRIu64 ",%u\n", ts, fmt_time(ts).c_str(),
                     symbol_of_[loc].c_str(), fmt_px(t.bid_price).c_str(), t.bid_shares, t.bid_orders,
                     fmt_px(t.ask_price).c_str(), t.ask_shares, t.ask_orders);
    }

    void checkpoint() {
        for (std::size_t loc = 0; loc < trace_on_.size(); ++loc) {
            if (!trace_on_[loc]) continue;
            for (char side : {'B', 'S'}) {
                books_->for_each_level(static_cast<std::uint16_t>(loc), side, [&](const Level& l) {
                    std::fprintf(trace_, "D %" PRIu64 " %zu %c %u %" PRIu64 " %u", msg_index_, loc, side, l.price,
                                 l.shares, l.count);
                    books_->for_each_order(l, [&](const Order& o) {
                        std::fprintf(trace_, " %" PRIu64 ":%u", o.ref, o.shares);
                    });
                    std::fputc('\n', trace_);
                    ++trace_lines_;
                    return true;
                });
            }
        }
    }

    void snapshot_times(std::uint64_t ts) {
        while (next_time_ < opt_.times.size() && ts >= opt_.times[next_time_]) {
            std::vector<std::pair<std::string, TopOfBook>> row;
            for (const auto& s : opt_.symbols) {
                auto it = locate_of_.find(s);
                row.emplace_back(s, it == locate_of_.end() ? TopOfBook{} : books_->top(it->second));
            }
            tob_at_.push_back({opt_.times[next_time_], input_ended_, std::move(row)});
            ++next_time_;
        }
    }

    int report(const StreamStats& st, double secs) {
        std::uint64_t total = 0;
        for (auto c : type_counts_) total += c;
        std::printf("== itchbook replay ==\n");
        std::printf("input                 %s\n", opt_.input.c_str());
        std::printf("level container       %s\n", opt_.levels.c_str());
        std::printf("bytes read            %" PRIu64 "\n", st.bytes);
        std::printf("messages              %" PRIu64 "\n", msg_index_);
        std::printf("trailing partial bytes %" PRIu64 "\n", st.trailing_bytes);
        std::printf("wall time             %.2f s (%.2f M msg/s, includes input and reporting work)\n", secs,
                    secs > 0 ? static_cast<double>(msg_index_) / secs / 1e6 : 0.0);
        std::printf("first/last timestamp  %s / %s\n", fmt_time(first_ts()).c_str(), fmt_time(last_ts_).c_str());
        if (trace_) std::printf("trace lines written   %" PRIu64 "\n", trace_lines_);

        std::printf("\n-- messages by type --\n");
        for (int c = 0; c < 256; ++c)
            if (type_counts_[c]) std::printf("  %c  %12" PRIu64 "\n", c, type_counts_[c]);
        std::printf("  total %10" PRIu64 "\n", total);

        std::printf("\n-- system events --\n");
        for (const auto& [code, ts] : system_events_) std::printf("  %c at %s\n", code, fmt_time(ts).c_str());

        std::printf("\n-- book --\n");
        std::printf("stock directory msgs  %" PRIu64 "\n", directory_count_);
        std::printf("live orders at end    %zu\n", books_->live_orders());
        std::printf("live price levels end %zu\n", books_->live_levels());
        std::printf("peak live orders      %zu\n", books_->peak_orders());
        std::printf("order map rehashes    %zu\n", books_->map_rehashes());
        std::printf("pool reallocations    %zu\n", books_->pool_grows());

        std::uint64_t anomalies = bad_length_ + unknown_type_ + ts_regressions_ + symbol_mismatches_ +
                                  book_msgs_unknown_locate_ + directory_conflicts_ + bad_printable_ + st.trailing_bytes;
        std::printf("\n-- invariants (all should be 0) --\n");
        const char types[] = {'A', 'F', 'E', 'C', 'X', 'D', 'U'};
        for (char t : types) {
            const auto& r = results_[static_cast<unsigned char>(t)];
            std::printf("  %c:", t);
            for (std::size_t i = 1; i < r.size(); ++i) {
                std::printf(" %s=%" PRIu64, to_string(static_cast<Result>(i)), r[i]);
                anomalies += r[i];
            }
            std::printf("\n");
        }
        std::printf("  bad length msgs              %" PRIu64 "\n", bad_length_);
        std::printf("  unknown type msgs            %" PRIu64 "\n", unknown_type_);
        std::printf("  timestamp regressions        %" PRIu64 "\n", ts_regressions_);
        std::printf("  stock field != directory     %" PRIu64 "\n", symbol_mismatches_);
        std::printf("  book msgs for unknown locate %" PRIu64 "\n", book_msgs_unknown_locate_);
        std::printf("  directory locate conflicts   %" PRIu64 "\n", directory_conflicts_);
        std::printf("  C printable not Y/N          %" PRIu64 "\n", bad_printable_);
        std::printf("  audits run / failed          %" PRIu64 " / %zu\n", audits_, audit_failures_.size());
        for (const auto& f : audit_failures_) std::printf("    AUDIT FAILURE %s\n", f.c_str());
        for (const auto& e : anomaly_examples_) std::printf("    example: %s\n", e.c_str());
        std::printf("\n-- informational --\n");
        std::printf("  P trade msgs with nonzero ref %" PRIu64 "\n", trade_nonzero_ref_);
        std::printf("  broken trade msgs             %" PRIu64 "\n", broken_trades_);
        std::printf("  book msgs leaving a book locked  (pre 09:30 / regular / post 16:00): %" PRIu64 " / %" PRIu64
                    " / %" PRIu64 "\n", locked_after_msg_[0], locked_after_msg_[1], locked_after_msg_[2]);
        std::printf("  book msgs leaving a book crossed (pre 09:30 / regular / post 16:00): %" PRIu64 " / %" PRIu64
                    " / %" PRIu64 "\n", crossed_after_msg_[0], crossed_after_msg_[1], crossed_after_msg_[2]);
        std::printf("  crossed while in trading state T / halted, paused or quote-only: %" PRIu64 " / %" PRIu64 "\n",
                    crossed_while_trading_, crossed_while_not_trading_);
        std::printf("  crossed in state T after a non-printable 'C' (cross executions) / after anything else: %" PRIu64
                    " / %" PRIu64 "\n", crossed_trading_cross_exec_, crossed_trading_other_);
        std::printf("  symbols ever crossed             (pre 09:30 / regular / post 16:00): %zu / %zu / %zu\n",
                    crossed_locates_[0].size(), crossed_locates_[1].size(), crossed_locates_[2].size());
        for (const auto& e : crossed_examples_) std::printf("    regular-hours example: %s\n", e.c_str());
        for (const auto& e : crossed_in_t_examples_) std::printf("    crossed in state T: %s\n", e.c_str());
        for (const auto& e : crossed_other_examples_)
            std::printf("    crossed in state T, not a cross execution: %s\n", e.c_str());

        std::printf("\n-- symbols --\n");
        std::printf("  %-6s %6s %12s %12s %12s %12s %12s %12s %12s %10s %10s %10s %10s %10s %10s %6s\n", "symbol",
                    "locate", "E", "C print", "C nonprint", "P", "cross open", "cross close", "total print",
                    "open px", "close px", "reg high", "reg low", "rl high", "rl low", "live");
        for (const auto& s : opt_.symbols) {
            auto it = locate_of_.find(s);
            if (it == locate_of_.end()) {
                std::printf("  %-6s not in stock directory\n", s.c_str());
                continue;
            }
            const std::uint16_t loc = it->second;
            const SymbolStats& x = stats_[loc];
            const std::uint64_t printed =
                x.exec_e + x.exec_c_print + x.trade_p + x.cross_open + x.cross_close + x.cross_other;
            std::size_t live = 0;
            for (char side : {'B', 'S'})
                books_->for_each_level(loc, side, [&](const Level& l) { live += l.count; return true; });
            std::printf("  %-6s %6u %12" PRIu64 " %12" PRIu64 " %12" PRIu64 " %12" PRIu64 " %12" PRIu64 " %12" PRIu64
                        " %12" PRIu64 " %10s %10s %10s %10s %10s %10s %6zu\n",
                        s.c_str(), loc, x.exec_e, x.exec_c_print, x.exec_c_noprint, x.trade_p, x.cross_open,
                        x.cross_close, printed, fmt_px(x.open_cross_px).c_str(), fmt_px(x.close_cross_px).c_str(),
                        fmt_px(x.reg_hi).c_str(), fmt_px(x.reg_lo).c_str(), fmt_px(x.rl_hi).c_str(),
                        fmt_px(x.rl_lo).c_str(), live);
        }

        if (!tob_at_.empty()) {
            std::printf("\n-- top of book as of time (state after the last message before it) --\n");
            for (const auto& [t, after_end, row] : tob_at_) {
                if (after_end)
                    std::printf("  %s: input ended at %s, showing end state\n", fmt_time(t).c_str(),
                                fmt_time(last_ts_).c_str());
                else
                    std::printf("  %s\n", fmt_time(t).c_str());
                for (const auto& [s, tob] : row)
                    std::printf("    %-6s bid %10s x %-8" PRIu64 " (%u orders)   ask %10s x %-8" PRIu64 " (%u orders)\n",
                                s.c_str(), fmt_px(tob.bid_price).c_str(), tob.bid_shares, tob.bid_orders,
                                fmt_px(tob.ask_price).c_str(), tob.ask_shares, tob.ask_orders);
            }
        }
        std::printf("\nRESULT %s: %" PRIu64 " anomalies, %zu audit failures\n",
                    (anomalies == 0 && audit_failures_.empty()) ? "PASS" : "FAIL", anomalies, audit_failures_.size());
        return (anomalies == 0 && audit_failures_.empty()) ? 0 : 1;
    }

    std::uint64_t first_ts() const { return system_events_.empty() ? 0 : system_events_.front().second; }

    const Options& opt_;
    std::unique_ptr<Books> books_;
    BookApplier<Books> applier_{*books_};

    std::uint64_t msg_index_ = 0;
    std::array<std::uint64_t, 256> type_counts_{};
    std::array<std::array<std::uint64_t, static_cast<std::size_t>(Result::count_)>, 256> results_{};
    std::uint64_t last_ts_ = 0, ts_regressions_ = 0;
    std::uint64_t bad_length_ = 0, unknown_type_ = 0;
    std::uint64_t symbol_mismatches_ = 0, book_msgs_unknown_locate_ = 0, directory_conflicts_ = 0;
    std::uint64_t directory_count_ = 0, bad_printable_ = 0, trade_nonzero_ref_ = 0, broken_trades_ = 0;
    static constexpr std::uint64_t kOpen = 34200ull * 1'000'000'000ull;   // 09:30
    static constexpr std::uint64_t kClose = 57600ull * 1'000'000'000ull;  // 16:00
    std::uint64_t locked_after_msg_[3] = {}, crossed_after_msg_[3] = {};
    std::set<std::uint16_t> crossed_locates_[3];
    std::vector<std::string> crossed_examples_, crossed_in_t_examples_, crossed_other_examples_;
    std::uint64_t crossed_while_trading_ = 0, crossed_while_not_trading_ = 0;
    std::uint64_t crossed_trading_cross_exec_ = 0, crossed_trading_other_ = 0;
    // Spec 1.2.2: a security absent from the pre-open Trading Action spin is
    // to be treated as halted, so the state starts as 'H' until a 'T' arrives.
    std::vector<char> trading_state_ = std::vector<char>(65536, 'H');
    std::uint64_t audits_ = 0;
    std::uint64_t trace_lines_ = 0;
    std::vector<std::string> audit_failures_;
    std::vector<std::string> anomaly_examples_;
    std::vector<std::pair<char, std::uint64_t>> system_events_;

    std::vector<std::string> symbol_of_ = std::vector<std::string>(65536);
    std::vector<std::string> raw_symbol_ = std::vector<std::string>(65536);
    std::unordered_map<std::string, std::uint16_t> locate_of_;
    std::unordered_map<std::string, bool> want_report_, want_csv_, want_trace_;
    std::vector<char> report_ = std::vector<char>(65536, 0);
    std::vector<char> csv_on_ = std::vector<char>(65536, 0);
    std::vector<char> trace_on_ = std::vector<char>(65536, 0);
    std::vector<SymbolStats> stats_ = std::vector<SymbolStats>(65536);

    std::size_t next_time_ = 0;
    struct TobRow {
        std::uint64_t time;
        bool after_end;
        std::vector<std::pair<std::string, TopOfBook>> row;
    };
    std::vector<TobRow> tob_at_;
    bool input_ended_ = false;

    std::FILE* csv_ = nullptr;
    std::FILE* trace_ = nullptr;
};

void usage() {
    std::fprintf(stderr,
                 "usage: replay [--symbols A,B] [--times HH:MM:SS,..] [--csv FILE] [--csv-symbols A,B]\n"
                 "              [--trace FILE] [--trace-symbols A,B|ALL] [--checkpoint-every N]\n"
                 "              [--audit-every N] [--max-messages N] [--levels vector|binary|map] [FILE|-]\n");
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    std::vector<std::string> times{"09:35:00", "12:00:00", "15:59:59"};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (a == "--symbols") opt.symbols = split(next());
        else if (a == "--times") times = split(next());
        else if (a == "--csv") opt.csv = next();
        else if (a == "--csv-symbols") opt.csv_symbols = split(next());
        else if (a == "--trace") opt.trace = next();
        else if (a == "--trace-symbols") {
            const std::string v = next();
            if (v == "ALL") opt.trace_all = true; else opt.trace_symbols = split(v);
        } else if (a == "--checkpoint-every") opt.checkpoint_every = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--audit-every") opt.audit_every = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--max-messages") opt.max_messages = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--levels") opt.levels = next();
        else if (a == "-h" || a == "--help") usage();
        else if (!a.empty() && a[0] == '-' && a != "-") usage();
        else opt.input = a;
    }
    for (const auto& t : times) opt.times.push_back(parse_time(t));
    std::sort(opt.times.begin(), opt.times.end());

    if (opt.levels == "vector") return Replayer<VectorLevels>(opt).run();
    if (opt.levels == "binary") return Replayer<VectorBinaryLevels>(opt).run();
    if (opt.levels == "map") return Replayer<MapLevels>(opt).run();
    usage();
}
