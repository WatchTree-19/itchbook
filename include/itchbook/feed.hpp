// Glue between the ITCH decoder and the order books, plus a streaming reader
// for a raw ITCH file (the 2-byte length prefixed format of the Nasdaq sample
// files) on any file descriptor, including a pipe from gunzip.
#pragma once

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "itchbook/book.hpp"
#include "itchbook/itch.hpp"

namespace itchbook {

// Applies the seven order book messages of ITCH 5.0 to an ItchBooks.
template <class Books>
struct BookApplier {
    Books& books;

    Result apply(const itch::AddOrder& m) {
        return books.add(m.stock_locate(), m.order_reference_number(), m.buy_sell_indicator(), m.shares(),
                         m.price());
    }
    Result apply(const itch::AddOrderMpid& m) {
        return books.add(m.stock_locate(), m.order_reference_number(), m.buy_sell_indicator(), m.shares(),
                         m.price());
    }
    Result apply(const itch::OrderExecuted& m) {
        return books.execute(m.stock_locate(), m.order_reference_number(), m.executed_shares());
    }
    // The order stays at its display price; execution_price is the print.
    Result apply(const itch::OrderExecutedWithPrice& m) {
        return books.execute(m.stock_locate(), m.order_reference_number(), m.executed_shares());
    }
    Result apply(const itch::OrderCancel& m) {
        return books.cancel(m.stock_locate(), m.order_reference_number(), m.cancelled_shares());
    }
    Result apply(const itch::OrderDelete& m) { return books.remove(m.stock_locate(), m.order_reference_number()); }
    Result apply(const itch::OrderReplace& m) {
        return books.replace(m.stock_locate(), m.original_order_reference_number(), m.new_order_reference_number(),
                             m.shares(), m.price());
    }
};

// Minimal handler: builds the books and counts results. Used by benchmarks
// and tests; the replay tool has a richer handler with the same core.
template <class Books>
struct BookHandler {
    BookApplier<Books> applier;
    std::array<std::uint64_t, static_cast<std::size_t>(Result::count_)> results{};
    std::uint64_t messages = 0;

    explicit BookHandler(Books& b) : applier{b} {}

    template <class M>
    void on(const M& m)
        requires requires(BookApplier<Books>& a) { a.apply(m); }
    {
        ++results[static_cast<std::size_t>(applier.apply(m))];
    }
    [[nodiscard]] std::uint64_t anomalies() const noexcept {
        std::uint64_t n = 0;
        for (std::size_t i = 1; i < results.size(); ++i) n += results[i];
        return n;
    }
};

// True for the seven order book message types. All of them carry an order
// reference number at offset 11 (for 'U' it is the original reference).
[[nodiscard]] inline bool is_book_message(char t) noexcept {
    return t == 'A' || t == 'F' || t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U';
}

// Like itch::dispatch_buffer, but while processing message i it prefetches
// the order map slot of message i + 3*Step, the order of message i + 2*Step
// and the level and queue neighbours of message i + Step. Every access is a
// hint; results are identical to dispatch_buffer. Only for streams that are
// available ahead of time (file replay, or the messages of one packet).
template <std::size_t Step = 4, class H, class Books>
std::size_t dispatch_buffer_lookahead(const char* buf, std::size_t n, H& h, const Books& books) {
    // Cursors walking ahead of the processing cursor, one message per step.
    // step() advances past one message and returns true if it was a well
    // formed book message, leaving its order reference in ref.
    struct Cursor {
        std::size_t off = 0;
        std::uint64_t ref = 0;
        bool step(const char* b, std::size_t len) {
            if (len - off < 2) return false;
            const std::size_t l = itch::be16(b + off);
            if (len - off - 2 < l) return false;
            const char* msg = b + off + 2;
            off += 2 + l;
            if (l < 19 || !is_book_message(msg[0])) return false;
            ref = itch::be64(msg + 11);
            return true;
        }
    };
    Cursor c1, c2, c3;  // 3*Step, 2*Step and Step messages ahead
    for (std::size_t i = 0; i < 3 * Step; ++i)
        if (c1.step(buf, n)) books.prefetch_slot(c1.ref);
    for (std::size_t i = 0; i < 2 * Step; ++i)
        if (c2.step(buf, n)) books.prefetch_order(c2.ref);
    for (std::size_t i = 0; i < Step; ++i)
        if (c3.step(buf, n)) books.prefetch_neighbours(c3.ref);
    std::size_t off = 0;
    while (n - off >= 2) {
        const std::size_t len = itch::be16(buf + off);
        if (n - off - 2 < len) break;
        if (c1.step(buf, n)) books.prefetch_slot(c1.ref);
        if (c2.step(buf, n)) books.prefetch_order(c2.ref);
        if (c3.step(buf, n)) books.prefetch_neighbours(c3.ref);
        itch::dispatch(buf + off + 2, len, h);
        off += 2 + len;
    }
    return off;
}

struct StreamStats {
    std::uint64_t bytes = 0;           // bytes read from the descriptor
    std::uint64_t consumed = 0;        // bytes making up complete messages
    std::uint64_t trailing_bytes = 0;  // incomplete message at end of input
};

// Reads length-prefixed messages until EOF and dispatches each. rd(ptr, n)
// behaves like read(2): it returns the number of bytes read, 0 at end of
// input, or -1 with errno set. If stop() returns true after a chunk is
// processed, reading stops early. The buffer always holds at least one
// maximum-size frame (2 + 65535 bytes), so a partial message can always be
// completed by the next read.
template <class H, class Stop, class ReadFn>
StreamStats read_stream_with(ReadFn&& rd, H& h, Stop&& stop, std::size_t buf_size = std::size_t{1} << 22) {
    constexpr std::size_t kMaxFrame = 2 + 65535;
    std::vector<char> buf(buf_size < kMaxFrame ? kMaxFrame : buf_size);
    std::size_t have = 0;
    StreamStats st;
    for (;;) {
        const ssize_t r = rd(buf.data() + have, buf.size() - have);
        if (r < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
        }
        if (r == 0) break;
        st.bytes += static_cast<std::uint64_t>(r);
        have += static_cast<std::size_t>(r);
        const std::size_t used = itch::dispatch_buffer(buf.data(), have, h);
        st.consumed += used;
        std::memmove(buf.data(), buf.data() + used, have - used);
        have -= used;
        if (stop()) return st;
    }
    st.trailing_bytes = have;
    return st;
}

template <class H, class Stop>
StreamStats read_stream(int fd, H& h, Stop&& stop, std::size_t buf_size = std::size_t{1} << 22) {
    return read_stream_with([fd](char* p, std::size_t n) { return ::read(fd, p, n); }, h, stop, buf_size);
}

template <class H>
StreamStats read_stream(int fd, H& h) {
    return read_stream(fd, h, [] { return false; });
}

}  // namespace itchbook
