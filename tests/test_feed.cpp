// Feed-level tests on the committed synthetic ITCH file: the plain and the
// lookahead dispatch loops must build identical books, and the stream reader
// must handle messages split across read() calls.
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "doctest.h"
#include "itchbook/feed.hpp"

using namespace itchbook;

namespace {

std::vector<char> synthetic() {
    std::ifstream f(std::string(ITCHBOOK_SOURCE_DIR) + "/tests/data/synthetic.itch", std::ios::binary);
    REQUIRE(f.good());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

using Books = ItchBooks<VectorLevels>;

// Every order of every book, in priority order, as text.
std::string dump(const Books& b) {
    std::string s;
    for (std::uint16_t loc = 0; loc < 16; ++loc)
        for (char side : {'B', 'S'})
            b.for_each_level(loc, side, [&](const Level& l) {
                s += std::to_string(loc) + side + std::to_string(l.price) + ":";
                b.for_each_order(l, [&](const Order& o) {
                    s += std::to_string(o.ref) + "/" + std::to_string(o.shares) + ",";
                });
                s += "\n";
                return true;
            });
    return s;
}

}  // namespace

TEST_CASE("lookahead dispatch builds the same books as plain dispatch") {
    const std::vector<char> buf = synthetic();
    Books a(1 << 16, 1 << 16), b(1 << 16, 1 << 16), c(1 << 16, 1 << 16);
    BookHandler<Books> ha(a), hb(b), hc(c);
    CHECK(itch::dispatch_buffer(buf.data(), buf.size(), ha) == buf.size());
    CHECK(dispatch_buffer_lookahead<4>(buf.data(), buf.size(), hb, b) == buf.size());
    CHECK(dispatch_buffer_lookahead<1>(buf.data(), buf.size(), hc, c) == buf.size());
    CHECK(ha.anomalies() == 0u);
    CHECK(ha.results == hb.results);
    CHECK(ha.results == hc.results);
    CHECK(a.live_orders() > 1000u);
    CHECK(a.live_orders() == b.live_orders());
    const std::string da = dump(a);
    CHECK(da == dump(b));
    CHECK(da == dump(c));
    CHECK(a.audit().empty());
    CHECK(b.audit().empty());
}

TEST_CASE("stream reader: messages split across short reads") {
    const std::vector<char> buf = synthetic();
    const std::size_t n = 300000;  // not on a message boundary
    std::size_t pos = 0;
    std::uint32_t k = 1;
    // Returns 1..7 bytes per call, so almost every message is split.
    auto short_reads = [&](char* p, std::size_t cap) -> ssize_t {
        k = k * 1103515245u + 12345u;
        std::size_t want = 1 + (k >> 16) % 7;
        if (want > cap) want = cap;
        if (want > n - pos) want = n - pos;
        std::memcpy(p, buf.data() + pos, want);
        pos += want;
        return static_cast<ssize_t>(want);
    };
    Books a(1 << 14, 1 << 14), b(1 << 14, 1 << 14);
    BookHandler<Books> ha(a), hb(b);
    const StreamStats st = read_stream_with(short_reads, ha, [] { return false; }, 16);
    const std::size_t whole = itch::dispatch_buffer(buf.data(), n, hb);
    CHECK(st.bytes == n);
    CHECK(st.consumed == whole);
    CHECK(st.trailing_bytes == n - whole);
    CHECK(st.trailing_bytes > 0u);
    CHECK(ha.results == hb.results);
    CHECK(dump(a) == dump(b));
}

TEST_CASE("stream reader: pipe") {
    const std::vector<char> buf = synthetic();
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const std::size_t n = 50000;  // fits in the default pipe buffer
    REQUIRE(::write(fds[1], buf.data(), n) == static_cast<ssize_t>(n));
    ::close(fds[1]);
    Books a(1 << 12, 1 << 12);
    BookHandler<Books> ha(a);
    const StreamStats st = read_stream(fds[0], ha);
    ::close(fds[0]);
    CHECK(st.bytes == n);
    CHECK(st.consumed + st.trailing_bytes == n);
    CHECK(ha.anomalies() == 0u);
    CHECK(a.live_orders() > 0u);
}

namespace {

// Minimal big-endian message builder for the applier test (offsets from the
// spec tables, written out here rather than taken from itch.hpp).
struct Msg {
    std::vector<char> b;
    Msg(char type, std::size_t size, std::uint16_t locate) : b(size, '\0') {
        b[0] = type;
        put(1, 2, locate);
        put(5, 6, 36000ull * 1'000'000'000ull);
    }
    Msg& put(std::size_t off, std::size_t width, std::uint64_t v) {
        for (std::size_t i = 0; i < width; ++i) b[off + i] = static_cast<char>((v >> (8 * (width - 1 - i))) & 0xFF);
        return *this;
    }
    Msg& ch(std::size_t off, char c) {
        b[off] = c;
        return *this;
    }
};

Msg add(std::uint16_t loc, std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t px, bool mpid = false) {
    Msg m(mpid ? 'F' : 'A', mpid ? 40 : 36, loc);
    m.put(11, 8, ref).ch(19, side).put(20, 4, shares).put(32, 4, px);
    for (std::size_t i = 0; i < 8; ++i) m.b[24 + i] = "TEST    "[i];
    return m;
}

}  // namespace

TEST_CASE("BookApplier maps every order book message to the right book operation") {
    Books b(1 << 10, 1 << 10);
    BookHandler<Books> h(b);
    auto send = [&](const Msg& m) { REQUIRE(itch::dispatch(m.b.data(), m.b.size(), h) == itch::Status::ok); };
    const std::uint16_t L = 7;
    send(add(L, 1, 'B', 300, 1000));
    send(add(L, 2, 'B', 200, 1000, true));  // 'F' behaves exactly like 'A'
    send(add(L, 3, 'S', 500, 1010));
    CHECK(b.top(L) == TopOfBook{1000, 500, 2, 1010, 500, 1});

    send(Msg('E', 31, L).put(11, 8, 1).put(19, 4, 100).put(23, 8, 77));  // executed 100 of ref 1
    CHECK(b.find(1)->shares == 200u);
    // Executed with price: shares come off, the order keeps its display price.
    send(Msg('C', 36, L).put(11, 8, 1).put(19, 4, 50).put(23, 8, 78).ch(31, 'Y').put(32, 4, 999));
    CHECK(b.find(1)->shares == 150u);
    CHECK(b.find(1)->price == 1000u);
    send(Msg('X', 23, L).put(11, 8, 3).put(19, 4, 125));  // partial cancel
    CHECK(b.find(3)->shares == 375u);
    CHECK(b.top(L) == TopOfBook{1000, 350, 2, 1010, 375, 1});

    // Replace ref 1 -> 10 at the same price: new ref, back of the queue.
    send(Msg('U', 35, L).put(11, 8, 1).put(19, 8, 10).put(27, 4, 60).put(31, 4, 1000));
    CHECK(b.find(1) == nullptr);
    REQUIRE(b.find(10) != nullptr);
    CHECK(b.find(10)->side == 'B');
    std::vector<std::uint64_t> q;
    b.for_each_level(L, 'B', [&](const Level& l) {
        b.for_each_order(l, [&](const Order& o) { q.push_back(o.ref); });
        return false;
    });
    CHECK(q == std::vector<std::uint64_t>{2, 10});

    send(Msg('D', 19, L).put(11, 8, 2));  // delete
    CHECK(b.find(2) == nullptr);
    CHECK(b.top(L) == TopOfBook{1000, 60, 1, 1010, 375, 1});
    // Executing the rest of an order removes it and its level.
    send(Msg('C', 36, L).put(11, 8, 10).put(19, 4, 60).put(23, 8, 79).ch(31, 'N').put(32, 4, 1005));
    CHECK(b.top(L) == TopOfBook{0, 0, 0, 1010, 375, 1});
    CHECK(h.anomalies() == 0u);
    CHECK(b.audit().empty());
}
