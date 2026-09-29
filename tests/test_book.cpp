// ITCH order book tests, run against every level container.
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "doctest.h"
#include "itchbook/book.hpp"

using namespace itchbook;

namespace {

template <class B>
std::vector<std::uint64_t> queue_of(const B& b, std::uint16_t loc, char side, std::uint32_t price) {
    std::vector<std::uint64_t> refs;
    b.for_each_level(loc, side, [&](const Level& l) {
        if (l.price == price) b.for_each_order(l, [&](const Order& o) { refs.push_back(o.ref); });
        return true;
    });
    return refs;
}

template <class B>
std::vector<std::pair<std::uint32_t, std::uint64_t>> depth_of(const B& b, std::uint16_t loc, char side) {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> d;
    b.for_each_level(loc, side, [&](const Level& l) {
        d.emplace_back(l.price, l.shares);
        return true;
    });
    return d;
}

using Depth = std::vector<std::pair<std::uint32_t, std::uint64_t>>;

}  // namespace

#define BOOK_TYPES ItchBooks<VectorLevels>, ItchBooks<VectorBinaryLevels>, ItchBooks<MapLevels>

TEST_CASE_TEMPLATE("empty book", B, BOOK_TYPES) {
    B b(64, 64);
    const TopOfBook t = b.top(1);
    CHECK(t.bid_price == 0u);
    CHECK(t.ask_price == 0u);
    CHECK(t.bid_shares == 0u);
    CHECK(b.live_orders() == 0u);
    CHECK(b.find(1) == nullptr);
    CHECK(b.execute(1, 1, 1) == Result::unknown_ref);
    CHECK(b.cancel(1, 1, 1) == Result::unknown_ref);
    CHECK(b.remove(1, 1) == Result::unknown_ref);
    CHECK(b.replace(1, 1, 2, 10, 10) == Result::unknown_ref);
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("add builds levels, aggregates and best prices", B, BOOK_TYPES) {
    B b(64, 64);
    CHECK(b.add(7, 1, 'B', 100, 10000) == Result::ok);
    CHECK(b.add(7, 2, 'B', 200, 10100) == Result::ok);
    CHECK(b.add(7, 3, 'B', 50, 10100) == Result::ok);
    CHECK(b.add(7, 4, 'S', 300, 10300) == Result::ok);
    CHECK(b.add(7, 5, 'S', 10, 10200) == Result::ok);
    const TopOfBook t = b.top(7);
    CHECK(t.bid_price == 10100u);
    CHECK(t.bid_shares == 250u);
    CHECK(t.bid_orders == 2u);
    CHECK(t.ask_price == 10200u);
    CHECK(t.ask_shares == 10u);
    CHECK(t.ask_orders == 1u);
    CHECK(depth_of(b, 7, 'B') == Depth{{10100, 250}, {10000, 100}});
    CHECK(depth_of(b, 7, 'S') == Depth{{10200, 10}, {10300, 300}});
    CHECK(b.live_orders() == 5u);
    CHECK(b.top(8) == TopOfBook{});  // other locates untouched
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("FIFO within a level", B, BOOK_TYPES) {
    B b(64, 64);
    for (std::uint64_t r = 10; r < 15; ++r) CHECK(b.add(1, r, 'S', 100, 500) == Result::ok);
    CHECK(queue_of(b, 1, 'S', 500) == std::vector<std::uint64_t>{10, 11, 12, 13, 14});
    // Removing from the middle, head and tail keeps the others in order.
    CHECK(b.remove(1, 12) == Result::ok);
    CHECK(b.remove(1, 10) == Result::ok);
    CHECK(b.remove(1, 14) == Result::ok);
    CHECK(queue_of(b, 1, 'S', 500) == std::vector<std::uint64_t>{11, 13});
    CHECK(b.add(1, 15, 'S', 100, 500) == Result::ok);
    CHECK(queue_of(b, 1, 'S', 500) == std::vector<std::uint64_t>{11, 13, 15});
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("partial execution reduces shares and keeps position", B, BOOK_TYPES) {
    B b(64, 64);
    b.add(1, 1, 'B', 300, 100);
    b.add(1, 2, 'B', 200, 100);
    CHECK(b.execute(1, 1, 100) == Result::ok);
    REQUIRE(b.find(1) != nullptr);
    CHECK(b.find(1)->shares == 200u);
    CHECK(b.top(1).bid_shares == 400u);
    CHECK(b.top(1).bid_orders == 2u);
    CHECK(queue_of(b, 1, 'B', 100) == std::vector<std::uint64_t>{1, 2});
    // Executions are cumulative.
    CHECK(b.execute(1, 1, 150) == Result::ok);
    CHECK(b.find(1)->shares == 50u);
    CHECK(b.top(1).bid_shares == 250u);
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("execution to zero removes the order, then the level", B, BOOK_TYPES) {
    B b(64, 64);
    b.add(1, 1, 'S', 100, 200);
    b.add(1, 2, 'S', 100, 200);
    b.add(1, 3, 'S', 500, 201);
    CHECK(b.execute(1, 1, 100) == Result::ok);  // exactly the order's size
    CHECK(b.find(1) == nullptr);
    CHECK(b.top(1).ask_price == 200u);
    CHECK(b.top(1).ask_orders == 1u);
    CHECK(b.execute(1, 2, 100) == Result::ok);  // exactly the level's size
    CHECK(b.top(1).ask_price == 201u);
    CHECK(b.top(1).ask_shares == 500u);
    CHECK(depth_of(b, 1, 'S') == Depth{{201, 500}});
    CHECK(b.live_levels() == 1u);
    CHECK(b.execute(1, 3, 500) == Result::ok);
    CHECK(b.top(1) == TopOfBook{});
    CHECK(b.live_orders() == 0u);
    CHECK(b.live_levels() == 0u);
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("over-execution and over-cancel are rejected, book unchanged", B, BOOK_TYPES) {
    B b(64, 64);
    b.add(3, 1, 'B', 100, 50);
    CHECK(b.execute(3, 1, 101) == Result::over_execute);
    CHECK(b.cancel(3, 1, 101) == Result::over_cancel);
    CHECK(b.find(1)->shares == 100u);
    CHECK(b.top(3).bid_shares == 100u);
    CHECK(b.execute(3, 999, 1) == Result::unknown_ref);
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("cancel: partial, exact and cumulative", B, BOOK_TYPES) {
    B b(64, 64);
    b.add(2, 1, 'S', 1000, 70);
    b.add(2, 2, 'S', 10, 70);
    CHECK(b.cancel(2, 1, 400) == Result::ok);
    CHECK(b.find(1)->shares == 600u);
    CHECK(b.top(2).ask_shares == 610u);
    CHECK(queue_of(b, 2, 'S', 70) == std::vector<std::uint64_t>{1, 2});  // no priority change
    CHECK(b.cancel(2, 1, 600) == Result::ok);
    CHECK(b.find(1) == nullptr);
    CHECK(b.top(2).ask_shares == 10u);
    CHECK(b.top(2).ask_orders == 1u);
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("delete", B, BOOK_TYPES) {
    B b(64, 64);
    b.add(2, 1, 'B', 5, 70);
    b.add(2, 2, 'B', 6, 69);
    CHECK(b.remove(2, 1) == Result::ok);
    CHECK(b.top(2).bid_price == 69u);
    CHECK(b.remove(2, 1) == Result::unknown_ref);
    CHECK(b.remove(2, 2) == Result::ok);
    CHECK(b.top(2) == TopOfBook{});
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("replace: new ref, loses priority even at the same price", B, BOOK_TYPES) {
    B b(64, 64);
    b.add(4, 1, 'B', 100, 1000);
    b.add(4, 2, 'B', 100, 1000);
    b.add(4, 3, 'B', 100, 1000);
    CHECK(b.replace(4, 1, 11, 100, 1000) == Result::ok);  // same price, same size
    CHECK(b.find(1) == nullptr);
    REQUIRE(b.find(11) != nullptr);
    CHECK(b.find(11)->side == 'B');
    CHECK(b.find(11)->locate == 4u);
    CHECK(queue_of(b, 4, 'B', 1000) == std::vector<std::uint64_t>{2, 3, 11});
    CHECK(b.top(4).bid_shares == 300u);
    // Replace to a smaller size at the same price also goes to the back.
    CHECK(b.replace(4, 2, 12, 40, 1000) == Result::ok);
    CHECK(queue_of(b, 4, 'B', 1000) == std::vector<std::uint64_t>{3, 11, 12});
    CHECK(b.top(4).bid_shares == 240u);
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("replace to a new price moves the order and may empty a level", B, BOOK_TYPES) {
    B b(64, 64);
    b.add(4, 1, 'S', 100, 1000);
    b.add(4, 2, 'S', 100, 1001);
    CHECK(b.replace(4, 1, 21, 250, 1002) == Result::ok);
    CHECK(b.top(4).ask_price == 1001u);
    CHECK(depth_of(b, 4, 'S') == Depth{{1001, 100}, {1002, 250}});
    CHECK(b.find(21)->side == 'S');
    CHECK(b.replace(4, 2, 22, 5, 1002) == Result::ok);  // joins the tail of an existing level
    CHECK(queue_of(b, 4, 'S', 1002) == std::vector<std::uint64_t>{21, 22});
    CHECK(depth_of(b, 4, 'S') == Depth{{1002, 255}});
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("replace edge cases are rejected without side effects", B, BOOK_TYPES) {
    B b(64, 64);
    b.add(4, 1, 'B', 100, 1000);
    b.add(4, 2, 'B', 100, 999);
    CHECK(b.replace(4, 1, 2, 100, 1000) == Result::duplicate_ref);  // new ref already live
    CHECK(b.replace(4, 9, 10, 100, 1000) == Result::unknown_ref);
    CHECK(b.replace(5, 1, 10, 100, 1000) == Result::locate_mismatch);
    CHECK(b.replace(4, 1, 10, 0, 1000) == Result::zero_shares);
    CHECK(b.find(1) != nullptr);
    CHECK(b.find(10) == nullptr);
    CHECK(depth_of(b, 4, 'B') == Depth{{1000, 100}, {999, 100}});
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("duplicate add, zero shares, bad side, locate mismatch", B, BOOK_TYPES) {
    B b(64, 64);
    CHECK(b.add(1, 5, 'B', 100, 10) == Result::ok);
    CHECK(b.add(1, 5, 'S', 100, 11) == Result::duplicate_ref);
    CHECK(b.add(1, 6, 'B', 0, 10) == Result::zero_shares);
    CHECK(b.add(1, 7, 'X', 10, 10) == Result::bad_side);
    CHECK(b.execute(2, 5, 1) == Result::locate_mismatch);
    CHECK(b.cancel(2, 5, 1) == Result::locate_mismatch);
    CHECK(b.remove(2, 5) == Result::locate_mismatch);
    CHECK(b.live_orders() == 1u);
    CHECK(b.top(1).bid_shares == 100u);
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("the reserved reference 2^64-1 is refused everywhere", B, BOOK_TYPES) {
    B b(64, 64);
    const std::uint64_t r = ~std::uint64_t{0};
    CHECK(b.add(1, r, 'B', 100, 10) == Result::reserved_ref);
    CHECK(b.add(1, 1, 'B', 100, 10) == Result::ok);
    CHECK(b.replace(1, 1, r, 50, 10) == Result::reserved_ref);
    CHECK(b.execute(1, r, 1) == Result::unknown_ref);
    CHECK(b.cancel(1, r, 1) == Result::unknown_ref);
    CHECK(b.remove(1, r) == Result::unknown_ref);
    CHECK(b.replace(1, r, 2, 50, 10) == Result::unknown_ref);
    CHECK(b.find(r) == nullptr);
    CHECK(b.live_orders() == 1u);
    CHECK(b.top(1).bid_shares == 100u);
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("removing interior levels keeps the rest ordered", B, BOOK_TYPES) {
    B b(64, 64);
    std::uint64_t ref = 1;
    for (std::uint32_t px = 100; px < 110; ++px) b.add(9, ref++, 'B', px, px);  // shares == price
    // Remove 105 (interior), 109 (best), 100 (worst).
    b.remove(9, 6);
    b.remove(9, 10);
    b.remove(9, 1);
    CHECK(depth_of(b, 9, 'B') ==
          Depth{{108, 108}, {107, 107}, {106, 106}, {104, 104}, {103, 103}, {102, 102}, {101, 101}});
    CHECK(b.top(9).bid_price == 108u);
    CHECK(b.audit().empty());
}

TEST_CASE_TEMPLATE("clear keeps working afterwards", B, BOOK_TYPES) {
    B b(64, 64);
    b.add(1, 1, 'B', 10, 10);
    b.add(2, 2, 'S', 10, 10);
    b.clear();
    CHECK(b.live_orders() == 0u);
    CHECK(b.top(1) == TopOfBook{});
    CHECK(b.add(1, 1, 'B', 10, 10) == Result::ok);
    CHECK(b.top(1).bid_shares == 10u);
    CHECK(b.audit().empty());
}

// Randomised comparison against a deliberately naive model: every order in a
// std::map with an insertion sequence number, top of book and queues computed
// by brute force.
namespace {
struct NaiveOrder {
    std::uint16_t loc;
    char side;
    std::uint32_t price;
    std::uint32_t shares;
    std::uint64_t seq;
};
struct Naive {
    std::map<std::uint64_t, NaiveOrder> orders;
    std::uint64_t seq = 0;
    TopOfBook top(std::uint16_t loc) const {
        TopOfBook t;
        for (const auto& [r, o] : orders) {
            if (o.loc != loc) continue;
            if (o.side == 'B' && (t.bid_price == 0 || o.price > t.bid_price)) t.bid_price = o.price;
            if (o.side == 'S' && (t.ask_price == 0 || o.price < t.ask_price)) t.ask_price = o.price;
        }
        for (const auto& [r, o] : orders) {
            if (o.loc != loc) continue;
            if (o.side == 'B' && o.price == t.bid_price) { t.bid_shares += o.shares; ++t.bid_orders; }
            if (o.side == 'S' && o.price == t.ask_price) { t.ask_shares += o.shares; ++t.ask_orders; }
        }
        return t;
    }
    std::vector<std::uint64_t> queue(std::uint16_t loc, char side, std::uint32_t px) const {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> v;
        for (const auto& [r, o] : orders)
            if (o.loc == loc && o.side == side && o.price == px) v.emplace_back(o.seq, r);
        std::sort(v.begin(), v.end());
        std::vector<std::uint64_t> out;
        for (auto& [s, r] : v) out.push_back(r);
        return out;
    }
};
}  // namespace

TEST_CASE_TEMPLATE("randomised against a naive model", B, BOOK_TYPES) {
    B b(1024, 1024);
    Naive n;
    std::mt19937_64 rng(2024);
    std::uint64_t next_ref = 1;
    std::vector<std::uint64_t> refs;
    for (int step = 0; step < 30000; ++step) {
        const std::uint16_t loc = static_cast<std::uint16_t>(1 + rng() % 3);
        const int op = static_cast<int>(rng() % 10);
        // Mostly pick a live order; sometimes a dead or never-seen reference.
        std::uint64_t pick = 0;
        if (!n.orders.empty() && rng() % 20 != 0)
            pick = std::next(n.orders.begin(), static_cast<std::ptrdiff_t>(rng() % n.orders.size()))->first;
        else if (!refs.empty())
            pick = refs[rng() % refs.size()] + (rng() % 2) * 1000000;
        if ((op < 4 && n.orders.size() < 250) || n.orders.empty()) {
            const char side = (rng() % 2) ? 'B' : 'S';
            const std::uint32_t px = static_cast<std::uint32_t>(95 + rng() % 11);
            const std::uint32_t sh = static_cast<std::uint32_t>(1 + rng() % 500);
            const std::uint64_t r = next_ref++;
            REQUIRE(b.add(loc, r, side, sh, px) == Result::ok);
            n.orders[r] = {loc, side, px, sh, n.seq++};
            refs.push_back(r);
        } else {
            auto it = n.orders.find(pick);
            if (it == n.orders.end()) {
                CHECK(b.remove(loc, pick) == Result::unknown_ref);
                continue;
            }
            NaiveOrder& o = it->second;
            const std::uint16_t oloc = o.loc;
            if (op < 6) {  // execute
                const std::uint32_t q = static_cast<std::uint32_t>(1 + rng() % o.shares);
                REQUIRE(b.execute(oloc, pick, q) == Result::ok);
                o.shares -= q;
                if (o.shares == 0) n.orders.erase(it);
            } else if (op < 7) {  // cancel
                const std::uint32_t q = static_cast<std::uint32_t>(1 + rng() % o.shares);
                REQUIRE(b.cancel(oloc, pick, q) == Result::ok);
                o.shares -= q;
                if (o.shares == 0) n.orders.erase(it);
            } else if (op < 8) {  // delete
                REQUIRE(b.remove(oloc, pick) == Result::ok);
                n.orders.erase(it);
            } else {  // replace
                const std::uint64_t r = next_ref++;
                const std::uint32_t px = static_cast<std::uint32_t>(95 + rng() % 11);
                const std::uint32_t sh = static_cast<std::uint32_t>(1 + rng() % 500);
                REQUIRE(b.replace(oloc, pick, r, sh, px) == Result::ok);
                NaiveOrder no = o;
                no.price = px;
                no.shares = sh;
                no.seq = n.seq++;
                n.orders.erase(it);
                n.orders[r] = no;
                refs.push_back(r);
            }
        }
        for (std::uint16_t l = 1; l <= 3; ++l) REQUIRE(b.top(l) == n.top(l));
        if (step % 500 == 0) {
            REQUIRE(b.audit().empty());
            for (std::uint16_t l = 1; l <= 3; ++l)
                for (char s : {'B', 'S'})
                    for (std::uint32_t px = 95; px <= 105; ++px) REQUIRE(queue_of(b, l, s, px) == n.queue(l, s, px));
        }
    }
    CHECK(b.live_orders() == n.orders.size());
}
