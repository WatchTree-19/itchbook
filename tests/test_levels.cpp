// The four level containers must behave identically: same lookups, same best
// price, same priority order, under random insert/erase churn.
#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include "doctest.h"
#include "itchbook/levels.hpp"

using namespace itchbook;

namespace {

template <class C>
std::vector<std::uint32_t> order_of(const C& c) {
    std::vector<std::uint32_t> v;
    c.for_each([&](std::uint32_t px, std::uint32_t) {
        v.push_back(px);
        return true;
    });
    return v;
}

template <bool Bid>
void churn(std::uint64_t seed, std::uint32_t lo, std::uint32_t hi) {
    const LevelConfig cfg{lo, hi};
    MapLevels<Bid> a(cfg);
    VectorLevels<Bid> b(cfg);
    VectorBinaryLevels<Bid> c(cfg);
    LadderLevels<Bid> d(cfg);
    std::set<std::uint32_t> ref;
    std::mt19937_64 rng(seed);
    std::uint32_t next_idx = 0;
    for (int i = 0; i < 20000; ++i) {
        const std::uint32_t px = lo + static_cast<std::uint32_t>(rng() % (hi - lo + 1));
        if (rng() % 2 == 0) {
            const std::uint32_t idx = next_idx;
            bool made = false;
            auto alloc = [&] {
                made = true;
                return idx;
            };
            const std::uint32_t ra = a.find_or_insert(px, alloc);
            const bool made_a = made;
            made = false;
            const std::uint32_t rb = b.find_or_insert(px, alloc);
            const bool made_b = made;
            made = false;
            const std::uint32_t rc = c.find_or_insert(px, alloc);
            const bool made_c = made;
            made = false;
            const std::uint32_t rd = d.find_or_insert(px, alloc);
            const bool made_d = made;
            const bool fresh = ref.insert(px).second;
            CHECK(made_a == fresh);
            CHECK(made_b == fresh);
            CHECK(made_c == fresh);
            CHECK(made_d == fresh);
            CHECK(ra == rb);
            CHECK(rb == rc);
            CHECK(rc == rd);
            if (fresh) ++next_idx;
        } else if (ref.count(px)) {
            a.erase(px);
            b.erase(px);
            c.erase(px);
            d.erase(px);
            ref.erase(px);
        }
        REQUIRE(a.size() == ref.size());
        REQUIRE(b.size() == ref.size());
        REQUIRE(c.size() == ref.size());
        REQUIRE(d.size() == ref.size());
        const std::uint32_t want = ref.empty() ? 0 : (Bid ? *ref.rbegin() : *ref.begin());
        REQUIRE(a.best_price() == want);
        REQUIRE(b.best_price() == want);
        REQUIRE(c.best_price() == want);
        REQUIRE(d.best_price() == want);
        REQUIRE(a.best() == b.best());
        REQUIRE(b.best() == c.best());
        REQUIRE(c.best() == d.best());
        CHECK(a.find(px) == d.find(px));
        CHECK(b.find(px) == c.find(px));
        if (i % 97 == 0) {
            const auto oa = order_of(a);
            CHECK(oa == order_of(b));
            CHECK(oa == order_of(c));
            CHECK(oa == order_of(d));
            std::vector<std::uint32_t> want_order(ref.begin(), ref.end());
            if (Bid) std::reverse(want_order.begin(), want_order.end());
            CHECK(oa == want_order);
        }
    }
}

}  // namespace

TEST_CASE("level containers agree: bids, narrow band") { churn<true>(1, 100, 160); }
TEST_CASE("level containers agree: asks, narrow band") { churn<false>(2, 100, 160); }
TEST_CASE("level containers agree: bids, wide band") { churn<true>(3, 1, 5000); }
TEST_CASE("level containers agree: asks, wide band") { churn<false>(4, 1, 5000); }
TEST_CASE("level containers agree: word boundaries") {
    churn<true>(5, 60, 200);
    churn<false>(6, 1, 128);
}

TEST_CASE("empty containers") {
    VectorLevels<true> v;
    MapLevels<false> m;
    LadderLevels<true> l(LevelConfig{1, 100});
    CHECK(v.best() == kNil);
    CHECK(m.best() == kNil);
    CHECK(l.best() == kNil);
    CHECK(v.find(5) == kNil);
    CHECK(l.find(0) == kNil);    // below range
    CHECK(l.find(101) == kNil);  // above range
    CHECK(v.best_price() == 0u);
    CHECK(l.empty());
}
