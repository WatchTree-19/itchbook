#include <cstdint>
#include <random>
#include <unordered_map>

#include "doctest.h"
#include "itchbook/hash_map.hpp"

using itchbook::BasicU64Map;
using itchbook::MapHash;

#define MAP_TYPES BasicU64Map<MapHash::fibonacci>, BasicU64Map<MapHash::identity>

TEST_CASE_TEMPLATE("U64Map basic insert/find/erase", M, MAP_TYPES) {
    M m(8);
    CHECK(m.size() == 0u);
    CHECK(m.find(1) == nullptr);
    CHECK(m.insert(1, 10));
    CHECK(m.insert(2, 20));
    CHECK_FALSE(m.insert(1, 99));  // duplicate keeps the old value
    REQUIRE(m.find(1) != nullptr);
    CHECK(*m.find(1) == 10u);
    CHECK(*m.find(2) == 20u);
    CHECK(m.size() == 2u);
    CHECK(m.erase(1));
    CHECK_FALSE(m.erase(1));
    CHECK(m.find(1) == nullptr);
    CHECK(*m.find(2) == 20u);
    CHECK(m.size() == 1u);
    CHECK(m.insert(0, 5));  // key 0 is an ordinary key
    CHECK(*m.find(0) == 5u);
    // 2^64-1 marks empty slots: it can never be stored or found.
    const std::uint64_t r = ~std::uint64_t{0};
    CHECK_FALSE(m.insert(r, 7));
    CHECK(m.find(r) == nullptr);
    CHECK_FALSE(m.erase(r));
    CHECK(m.size() == 2u);
}

TEST_CASE_TEMPLATE("U64Map erase keeps colliding probe runs reachable", M, MAP_TYPES) {
    // A small table and many keys force long probe runs that wrap around the
    // end of the slot array; backward-shift deletion must keep them intact.
    M m(16);
    std::unordered_map<std::uint64_t, std::uint32_t> ref;
    std::mt19937_64 rng(12345);
    for (int round = 0; round < 200000; ++round) {
        const std::uint64_t k = rng() % 64;  // tiny key space: heavy churn
        const int op = static_cast<int>(rng() % 3);
        if (op == 0) {
            const bool fresh = ref.find(k) == ref.end();
            CHECK(m.insert(k, static_cast<std::uint32_t>(round)) == fresh);
            if (fresh) ref[k] = static_cast<std::uint32_t>(round);
        } else if (op == 1) {
            CHECK(m.erase(k) == (ref.erase(k) == 1));
        } else {
            const std::uint32_t* v = m.find(k);
            auto it = ref.find(k);
            REQUIRE((v != nullptr) == (it != ref.end()));
            if (v) CHECK(*v == it->second);
        }
        REQUIRE(m.size() == ref.size());
    }
    for (const auto& [k, v] : ref) CHECK(*m.find(k) == v);
}

TEST_CASE_TEMPLATE("U64Map grows past its initial capacity and keeps every key", M, MAP_TYPES) {
    M m(16);
    const std::size_t cap0 = m.capacity();
    for (std::uint32_t i = 0; i < 10000; ++i) REQUIRE(m.insert(std::uint64_t{i} * 7919u + 3u, i));
    CHECK(m.capacity() > cap0);
    CHECK(m.rehash_count() > 0u);
    CHECK(m.size() * 2 <= m.capacity());
    for (std::uint32_t i = 0; i < 10000; ++i) {
        const std::uint32_t* v = m.find(std::uint64_t{i} * 7919u + 3u);
        REQUIRE(v != nullptr);
        CHECK(*v == i);
    }
    std::size_t n = 0;
    m.for_each([&](std::uint64_t, std::uint32_t) { ++n; });
    CHECK(n == 10000u);
    m.clear();
    CHECK(m.size() == 0u);
    CHECK(m.find(3) == nullptr);
}
