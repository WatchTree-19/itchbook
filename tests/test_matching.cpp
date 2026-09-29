// Matching engine tests, run against every level container.
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "itchbook/matching.hpp"

using namespace itchbook;

namespace {

// Records every event as a short string so tests can compare exact sequences.
struct Rec {
    std::vector<std::string> ev;
    void on_trade(const TradeEvent& t) {
        ev.push_back("T " + std::to_string(t.taker_id) + " " + std::to_string(t.maker_id) + " " +
                     std::to_string(t.price) + " " + std::to_string(t.qty));
    }
    void on_level(Side s, std::uint32_t px, std::uint64_t q, std::uint32_t n) {
        ev.push_back(std::string("L ") + (s == Side::buy ? "B " : "S ") + std::to_string(px) + " " +
                     std::to_string(q) + " " + std::to_string(n));
    }
    void on_rest(std::uint64_t id, Side s, std::uint32_t px, std::uint32_t q) {
        ev.push_back("A " + std::to_string(id) + (s == Side::buy ? " B " : " S ") + std::to_string(px) + " " +
                     std::to_string(q));
    }
    void on_amend(std::uint64_t id, std::uint32_t px, std::uint32_t q) {
        ev.push_back("M " + std::to_string(id) + " " + std::to_string(px) + " " + std::to_string(q));
    }
    void on_cancel(std::uint64_t id, std::uint32_t q, CancelReason r) {
        const char* c = r == CancelReason::user ? "U" : r == CancelReason::ioc_remainder ? "I" : "M";
        ev.push_back("C " + std::to_string(id) + " " + std::to_string(q) + " " + c);
    }
    void on_reject(std::uint64_t id, RejectReason r) {
        const char* c = r == RejectReason::duplicate_id ? "D"
                        : r == RejectReason::unknown_id ? "U"
                        : r == RejectReason::bad_quantity ? "Q"
                        : r == RejectReason::bad_price    ? "P" : "R";
        ev.push_back("J " + std::to_string(id) + " " + c);
    }
    std::vector<std::string> take() {
        auto out = std::move(ev);
        ev.clear();
        return out;
    }
};

using V = std::vector<std::string>;

NewOrder lim(std::uint64_t id, Side s, std::uint32_t px, std::uint32_t q, TimeInForce tif = TimeInForce::day) {
    return NewOrder{id, s, OrderType::limit, tif, px, q};
}
NewOrder mkt(std::uint64_t id, Side s, std::uint32_t q) { return NewOrder{id, s, OrderType::market, TimeInForce::day, 0, q}; }

template <class E>
std::vector<std::uint64_t> queue_at(const E& e, Side s, std::uint32_t px) {
    std::vector<std::uint64_t> ids;
    e.for_each_level(s, [&](const Level& l) {
        if (l.price == px) e.for_each_order(l, [&](const MatchOrder& o) { ids.push_back(o.id); });
        return true;
    });
    return ids;
}

template <template <bool> class L>
struct Fixture {
    Rec rec;
    EngineConfig cfg{1, 100000, 1024, 1024};
    MatchingEngine<Rec, L> eng{rec, cfg};
};

}  // namespace

#define ENGINE_TYPES Fixture<VectorLevels>, Fixture<VectorBinaryLevels>, Fixture<MapLevels>, Fixture<LadderLevels>

TEST_CASE_TEMPLATE("market order into an empty book is cancelled", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(mkt(1, Side::buy, 100));
    CHECK(f.rec.take() == V{"C 1 100 M"});
    CHECK(f.eng.live_orders() == 0u);
    f.eng.submit(lim(2, Side::sell, 50, 10, TimeInForce::ioc));
    CHECK(f.rec.take() == V{"C 2 10 I"});
    CHECK(f.eng.best_ask() == 0u);
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("non-crossing limits rest", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::buy, 100, 10));
    f.eng.submit(lim(2, Side::sell, 101, 20));
    CHECK(f.rec.take() == V{"A 1 B 100 10", "L B 100 10 1", "A 2 S 101 20", "L S 101 20 1"});
    CHECK(f.eng.best_bid() == 100u);
    CHECK(f.eng.best_ask() == 101u);
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("partial fill trades at the maker price, remainder rests", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::sell, 100, 30));
    f.rec.take();
    f.eng.submit(lim(2, Side::buy, 105, 50));  // price improvement: prints at 100
    CHECK(f.rec.take() == V{"T 2 1 100 30", "L S 100 0 0", "A 2 B 105 20", "L B 105 20 1"});
    CHECK(f.eng.best_ask() == 0u);
    CHECK(f.eng.best_bid() == 105u);
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("incoming order partially fills a resting order", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::buy, 100, 50));
    f.rec.take();
    f.eng.submit(lim(2, Side::sell, 100, 20));
    CHECK(f.rec.take() == V{"T 2 1 100 20", "L B 100 30 1"});
    REQUIRE(f.eng.find(1) != nullptr);
    CHECK(f.eng.find(1)->qty == 30u);
    CHECK(f.eng.find(2) == nullptr);
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("filling exactly a level removes it and exposes the next", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::sell, 100, 10));
    f.eng.submit(lim(2, Side::sell, 100, 15));
    f.eng.submit(lim(3, Side::sell, 101, 40));
    f.rec.take();
    f.eng.submit(lim(4, Side::buy, 100, 25));
    CHECK(f.rec.take() == V{"T 4 1 100 10", "L S 100 15 1", "T 4 2 100 15", "L S 100 0 0"});
    CHECK(f.eng.best_ask() == 101u);
    CHECK(f.eng.live_orders() == 1u);
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("market order sweeps several levels in price-time order", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::buy, 99, 10));
    f.eng.submit(lim(2, Side::buy, 100, 10));
    f.eng.submit(lim(3, Side::buy, 100, 10));
    f.eng.submit(lim(4, Side::buy, 98, 100));
    f.rec.take();
    f.eng.submit(mkt(5, Side::sell, 35));
    CHECK(f.rec.take() == V{"T 5 2 100 10", "L B 100 10 1", "T 5 3 100 10", "L B 100 0 0", "T 5 1 99 10",
                            "L B 99 0 0", "T 5 4 98 5", "L B 98 95 1"});
    CHECK(f.eng.best_bid() == 98u);
    // A market order larger than the book: fills everything, remainder cancelled.
    f.eng.submit(mkt(6, Side::sell, 200));
    CHECK(f.rec.take() == V{"T 6 4 98 95", "L B 98 0 0", "C 6 105 M"});
    CHECK(f.eng.live_orders() == 0u);
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("limit order stops at its limit price", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::sell, 100, 10));
    f.eng.submit(lim(2, Side::sell, 102, 10));
    f.rec.take();
    f.eng.submit(lim(3, Side::buy, 101, 30));
    CHECK(f.rec.take() == V{"T 3 1 100 10", "L S 100 0 0", "A 3 B 101 20", "L B 101 20 1"});
    CHECK(f.eng.best_ask() == 102u);
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("IOC: remainder cancelled, never rests", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::sell, 100, 10));
    f.rec.take();
    f.eng.submit(lim(2, Side::buy, 100, 25, TimeInForce::ioc));
    CHECK(f.rec.take() == V{"T 2 1 100 10", "L S 100 0 0", "C 2 15 I"});
    CHECK(f.eng.best_bid() == 0u);
    CHECK(f.eng.find(2) == nullptr);
    // IOC fully filled: no cancel event.
    f.eng.submit(lim(3, Side::sell, 100, 10));
    f.rec.take();
    f.eng.submit(lim(4, Side::buy, 100, 10, TimeInForce::ioc));
    CHECK(f.rec.take() == V{"T 4 3 100 10", "L S 100 0 0"});
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("time priority within a price", F, ENGINE_TYPES) {
    F f;
    for (std::uint64_t id = 1; id <= 4; ++id) f.eng.submit(lim(id, Side::sell, 200, 5));
    CHECK(queue_at(f.eng, Side::sell, 200) == std::vector<std::uint64_t>{1, 2, 3, 4});
    f.rec.take();
    f.eng.submit(lim(9, Side::buy, 200, 7));
    CHECK(f.rec.take() == V{"T 9 1 200 5", "L S 200 15 3", "T 9 2 200 2", "L S 200 13 3"});
    CHECK(queue_at(f.eng, Side::sell, 200) == std::vector<std::uint64_t>{2, 3, 4});
    CHECK(f.eng.find(2)->qty == 3u);
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("cancel", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::buy, 100, 10));
    f.eng.submit(lim(2, Side::buy, 100, 20));
    f.rec.take();
    f.eng.cancel(1);
    CHECK(f.rec.take() == V{"C 1 10 U", "L B 100 20 1"});
    f.eng.cancel(1);
    CHECK(f.rec.take() == V{"J 1 U"});
    f.eng.cancel(2);
    CHECK(f.rec.take() == V{"C 2 20 U", "L B 100 0 0"});
    CHECK(f.eng.best_bid() == 0u);
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("replace: reduce keeps priority, increase loses it", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::sell, 100, 50));
    f.eng.submit(lim(2, Side::sell, 100, 50));
    f.rec.take();
    f.eng.replace(1, 100, 30);  // reduce
    CHECK(f.rec.take() == V{"M 1 100 30", "L S 100 80 2"});
    CHECK(queue_at(f.eng, Side::sell, 100) == std::vector<std::uint64_t>{1, 2});
    f.eng.replace(1, 100, 30);  // unchanged: still keeps priority
    CHECK(f.rec.take() == V{"M 1 100 30", "L S 100 80 2"});
    CHECK(queue_at(f.eng, Side::sell, 100) == std::vector<std::uint64_t>{1, 2});
    f.eng.replace(1, 100, 31);  // increase: to the back
    CHECK(f.rec.take() == V{"L S 100 50 1", "A 1 S 100 31", "L S 100 81 2"});
    CHECK(queue_at(f.eng, Side::sell, 100) == std::vector<std::uint64_t>{2, 1});
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("replace to a new price loses priority and can trade", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::buy, 100, 10));
    f.eng.submit(lim(2, Side::buy, 101, 10));
    f.eng.submit(lim(3, Side::sell, 103, 5));
    f.rec.take();
    f.eng.replace(2, 101, 10);  // same qty same price: keeps priority, no requeue
    CHECK(f.rec.take() == V{"M 2 101 10", "L B 101 10 1"});
    f.eng.replace(1, 101, 10);  // price change: joins 101 behind order 2
    CHECK(f.rec.take() == V{"L B 100 0 0", "A 1 B 101 10", "L B 101 20 2"});
    CHECK(queue_at(f.eng, Side::buy, 101) == std::vector<std::uint64_t>{2, 1});
    f.eng.replace(2, 104, 8);  // crosses the ask at 103
    CHECK(f.rec.take() == V{"L B 101 10 1", "T 2 3 103 5", "L S 103 0 0", "A 2 B 104 3", "L B 104 3 1"});
    CHECK(f.eng.best_bid() == 104u);
    CHECK(f.eng.best_ask() == 0u);
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("rejects leave the book unchanged", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::buy, 100, 10));
    f.rec.take();
    f.eng.submit(lim(1, Side::buy, 100, 10));  // duplicate live id
    f.eng.submit(lim(2, Side::buy, 100, 0));   // zero qty
    f.eng.submit(lim(3, Side::buy, 0, 10));    // below band
    f.eng.submit(lim(4, Side::buy, 100001, 10));  // above band
    f.eng.submit(mkt(5, Side::sell, 0));
    f.eng.replace(9, 100, 10);
    f.eng.replace(1, 100, 0);
    f.eng.replace(1, 0, 10);
    CHECK(f.rec.take() == V{"J 1 D", "J 2 Q", "J 3 P", "J 4 P", "J 5 Q", "J 9 U", "J 1 Q", "J 1 P"});
    CHECK(f.eng.live_orders() == 1u);
    CHECK(f.eng.find(1)->qty == 10u);
    // An id can be reused once the order is gone.
    f.eng.cancel(1);
    f.rec.take();
    f.eng.submit(lim(1, Side::sell, 100, 1));
    CHECK(f.rec.take() == V{"A 1 S 100 1", "L S 100 1 1"});
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("the reserved id 2^64-1 is rejected, never traded or rested", F, ENGINE_TYPES) {
    F f;
    const std::uint64_t r = ~std::uint64_t{0};
    f.eng.submit(lim(1, Side::sell, 100, 10));
    f.rec.take();
    f.eng.submit(lim(r, Side::buy, 100, 5));  // would cross if accepted
    f.eng.cancel(r);
    f.eng.replace(r, 100, 1);
    CHECK(f.rec.take() == V{"J " + std::to_string(r) + " R", "J " + std::to_string(r) + " U",
                            "J " + std::to_string(r) + " U"});
    CHECK(f.eng.find(r) == nullptr);
    CHECK(f.eng.find(1)->qty == 10u);
    CHECK(f.eng.audit().empty());
}

TEST_CASE_TEMPLATE("book extremes of the price band", F, ENGINE_TYPES) {
    F f;
    f.eng.submit(lim(1, Side::buy, 1, 10));
    f.eng.submit(lim(2, Side::sell, 100000, 10));
    f.eng.submit(lim(3, Side::buy, 2, 10));
    f.eng.submit(lim(4, Side::sell, 99999, 10));
    CHECK(f.eng.best_bid() == 2u);
    CHECK(f.eng.best_ask() == 99999u);
    f.rec.take();
    f.eng.submit(mkt(5, Side::sell, 20));
    f.eng.submit(mkt(6, Side::buy, 20));
    CHECK(f.rec.take() == V{"T 5 3 2 10", "L B 2 0 0", "T 5 1 1 10", "L B 1 0 0", "T 6 4 99999 10",
                            "L S 99999 0 0", "T 6 2 100000 10", "L S 100000 0 0"});
    CHECK(f.eng.live_orders() == 0u);
    CHECK(f.eng.audit().empty());
}
