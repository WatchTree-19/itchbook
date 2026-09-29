// Order books reconstructed from TotalView-ITCH, one per stock locate.
//
// Storage:
//   * orders live in one pool (vector + free list) and are addressed by u32
//     index; the order reference -> index lookup is an open-addressing U64Map;
//   * each price level is a Level in a second pool, holding aggregate shares,
//     the order count and the head/tail of an intrusive doubly linked FIFO
//     threaded through Order::prev/next;
//   * each side of each book maps price -> level index through a pluggable
//     container (see levels.hpp). Best bid/ask is O(1) for every container.
//
// Semantics follow the ITCH 5.0 spec, section 1.4:
//   add       new order at the tail of its price level;
//   execute   (E and C) reduce displayed shares, remove at zero. An execution
//             with price does not move the order: the order keeps its display
//             price, the message price is only the print price;
//   cancel    (X) reduce displayed shares, remove at zero;
//   delete    (D) remove the order;
//   replace   (U) remove the original reference and add the new reference with
//             the original side and locate, at the tail of the new price level,
//             so it always loses time priority, even at an unchanged price.
//
// Messages that the spec says cannot happen (unknown reference, duplicate
// reference, executing or cancelling more shares than remain, locate that
// disagrees with the order, the reserved reference 2^64 - 1) are rejected with a Result code and leave the book
// unchanged, so a replay can count them instead of corrupting state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "itchbook/hash_map.hpp"
#include "itchbook/levels.hpp"
#include "itchbook/pool.hpp"

namespace itchbook {

enum class Result : std::uint8_t {
    ok,
    unknown_ref,
    duplicate_ref,
    over_execute,
    over_cancel,
    zero_shares,
    bad_side,
    locate_mismatch,
    reserved_ref,  // reference 2^64 - 1, which the order map cannot store
    count_
};

[[nodiscard]] constexpr const char* to_string(Result r) noexcept {
    switch (r) {
        case Result::ok: return "ok";
        case Result::unknown_ref: return "unknown_ref";
        case Result::duplicate_ref: return "duplicate_ref";
        case Result::over_execute: return "over_execute";
        case Result::over_cancel: return "over_cancel";
        case Result::zero_shares: return "zero_shares";
        case Result::bad_side: return "bad_side";
        case Result::locate_mismatch: return "locate_mismatch";
        case Result::reserved_ref: return "reserved_ref";
        default: return "?";
    }
}

struct Order {
    std::uint64_t ref;
    std::uint32_t shares;  // displayed shares still open
    std::uint32_t price;
    std::uint32_t prev;    // FIFO neighbours within the level (kNil at the ends)
    std::uint32_t next;
    std::uint32_t level;   // index into the level pool
    std::uint16_t locate;
    char side;             // 'B' or 'S'
};
static_assert(sizeof(Order) == 32);

struct TopOfBook {
    std::uint32_t bid_price = 0;  // 0 means the side is empty
    std::uint64_t bid_shares = 0;
    std::uint32_t bid_orders = 0;
    std::uint32_t ask_price = 0;
    std::uint64_t ask_shares = 0;
    std::uint32_t ask_orders = 0;
    friend bool operator==(const TopOfBook&, const TopOfBook&) = default;
};

// Hash is the order map's slot function (see hash_map.hpp). Identity hashing
// of Nasdaq's increasing references was measured and gave no gain on this
// machine, so the default is the pattern-proof Fibonacci hash.
template <template <bool> class LevelsT = VectorLevels, MapHash Hash = MapHash::fibonacci>
class ItchBooks {
public:
    using BidLevels = LevelsT<true>;
    using AskLevels = LevelsT<false>;

    struct Book {
        BidLevels bids;
        AskLevels asks;
    };

    static constexpr std::size_t kLocates = 65536;

    explicit ItchBooks(std::size_t expected_orders = std::size_t{1} << 22,
                       std::size_t expected_levels = std::size_t{1} << 20)
        : books_(kLocates), map_(expected_orders), orders_(expected_orders), levels_(expected_levels) {}

    // ------------------------------------------------------------ updates

    Result add(std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t shares,
               std::uint32_t price) {
        if (side != 'B' && side != 'S') [[unlikely]] return Result::bad_side;
        if (shares == 0) [[unlikely]] return Result::zero_shares;
        if (ref == U64Map::kEmpty) [[unlikely]] return Result::reserved_ref;
        const std::uint32_t oi = orders_.alloc();
        if (!map_.insert(ref, oi)) [[unlikely]] {
            orders_.release(oi);
            return Result::duplicate_ref;
        }
        Order& o = orders_[oi];
        o.ref = ref;
        o.shares = shares;
        o.price = price;
        o.locate = locate;
        o.side = side;
        link(oi);
        if (map_.size() > peak_orders_) peak_orders_ = map_.size();
        return Result::ok;
    }

    // Order Executed ('E') and Order Executed With Price ('C').
    Result execute(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) {
        return reduce(locate, ref, shares, Result::over_execute);
    }

    // Order Cancel ('X'): partial cancellation.
    Result cancel(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) {
        return reduce(locate, ref, shares, Result::over_cancel);
    }

    // Order Delete ('D').
    Result remove(std::uint16_t locate, std::uint64_t ref) {
        const std::uint32_t* slot = map_.find(ref);
        if (slot == nullptr) [[unlikely]] return Result::unknown_ref;
        const std::uint32_t oi = *slot;
        if (orders_[oi].locate != locate) [[unlikely]] return Result::locate_mismatch;
        kill(oi);
        return Result::ok;
    }

    // Order Replace ('U').
    Result replace(std::uint16_t locate, std::uint64_t old_ref, std::uint64_t new_ref,
                   std::uint32_t shares, std::uint32_t price) {
        const std::uint32_t* slot = map_.find(old_ref);
        if (slot == nullptr) [[unlikely]] return Result::unknown_ref;
        const std::uint32_t oi = *slot;
        Order& o = orders_[oi];
        if (o.locate != locate) [[unlikely]] return Result::locate_mismatch;
        if (shares == 0) [[unlikely]] return Result::zero_shares;
        if (new_ref == U64Map::kEmpty) [[unlikely]] return Result::reserved_ref;
        if (new_ref != old_ref && map_.find(new_ref) != nullptr) [[unlikely]] return Result::duplicate_ref;
        // Reuse the pool slot: unlink from the old level, relink at the tail
        // of the new one. The reference changes, so re-key the map.
        unlink(oi);
        map_.erase(old_ref);
        map_.insert(new_ref, oi);
        o.ref = new_ref;
        o.shares = shares;
        o.price = price;
        link(oi);
        return Result::ok;
    }

    // Removes every order and level but keeps all allocated capacity.
    void clear() {
        for (auto& b : books_) {
            b.bids.clear();
            b.asks.clear();
        }
        map_.clear();
        orders_.clear();
        levels_.clear();
        peak_orders_ = 0;
    }

    // ------------------------------------------------------------ queries

    [[nodiscard]] TopOfBook top(std::uint16_t locate) const noexcept {
        TopOfBook t;
        const Book& b = books_[locate];
        if (const std::uint32_t li = b.bids.best(); li != kNil) {
            const Level& l = levels_[li];
            t.bid_price = l.price;
            t.bid_shares = l.shares;
            t.bid_orders = l.count;
        }
        if (const std::uint32_t li = b.asks.best(); li != kNil) {
            const Level& l = levels_[li];
            t.ask_price = l.price;
            t.ask_shares = l.shares;
            t.ask_orders = l.count;
        }
        return t;
    }

    [[nodiscard]] const Book& book(std::uint16_t locate) const noexcept { return books_[locate]; }
    [[nodiscard]] const Level& level(std::uint32_t li) const noexcept { return levels_[li]; }
    [[nodiscard]] const Order& order_at(std::uint32_t oi) const noexcept { return orders_[oi]; }

    [[nodiscard]] const Order* find(std::uint64_t ref) const noexcept {
        const std::uint32_t* slot = map_.find(ref);
        return slot ? &orders_[*slot] : nullptr;
    }

    // Visits levels of one side best first; f(const Level&) returns false to stop.
    template <class F>
    void for_each_level(std::uint16_t locate, char side, F&& f) const {
        auto visit = [&](std::uint32_t, std::uint32_t li) { return f(levels_[li]); };
        if (side == 'B') books_[locate].bids.for_each(visit); else books_[locate].asks.for_each(visit);
    }

    // Visits the orders of a level oldest first.
    template <class F>
    void for_each_order(const Level& l, F&& f) const {
        for (std::uint32_t oi = l.head; oi != kNil; oi = orders_[oi].next) f(orders_[oi]);
    }

    // Prefetch hints for lookahead processing (see dispatch_buffer_lookahead).
    // They never change state, so correctness does not depend on them.
    void prefetch_slot(std::uint64_t ref) const noexcept { map_.prefetch(ref); }
    void prefetch_order(std::uint64_t ref) const noexcept {
        if (const std::uint32_t* s = map_.find(ref)) __builtin_prefetch(&orders_[*s]);
    }
    void prefetch_neighbours(std::uint64_t ref) const noexcept {
        if (const std::uint32_t* s = map_.find(ref)) {
            const Order& o = orders_[*s];
            __builtin_prefetch(&levels_[o.level]);
            if (o.prev != kNil) __builtin_prefetch(&orders_[o.prev]);
            if (o.next != kNil) __builtin_prefetch(&orders_[o.next]);
        }
    }

    [[nodiscard]] std::size_t live_orders() const noexcept { return map_.size(); }
    [[nodiscard]] std::size_t peak_orders() const noexcept { return peak_orders_; }
    [[nodiscard]] std::size_t live_levels() const noexcept { return levels_.live(); }
    [[nodiscard]] std::size_t map_rehashes() const noexcept { return map_.rehash_count(); }
    [[nodiscard]] std::size_t pool_grows() const noexcept { return orders_.grow_count() + levels_.grow_count(); }

    // Full structural consistency check of every book. Returns an empty string
    // if consistent, otherwise a description of the first problem found.
    // O(total orders + levels); meant for tests and periodic sampling.
    [[nodiscard]] std::string audit() const {
        std::size_t orders_seen = 0, levels_seen = 0;
        for (std::size_t loc = 0; loc < kLocates; ++loc) {
            const Book& b = books_[loc];
            std::string err;
            auto check_side = [&](const auto& side, char s) {
                bool first = true;
                std::uint32_t prev_px = 0;
                side.for_each([&](std::uint32_t px, std::uint32_t li) {
                    const Level& l = levels_[li];
                    ++levels_seen;
                    auto fail = [&](const char* what) {
                        err = std::string(what) + " locate=" + std::to_string(loc) + " side=" + s +
                              " price=" + std::to_string(px);
                        return false;
                    };
                    if (l.price != px) return fail("level price differs from key");
                    if (!first) {
                        const bool ordered = (s == 'B') ? px < prev_px : px > prev_px;
                        if (!ordered) return fail("levels out of priority order");
                    }
                    first = false;
                    prev_px = px;
                    if (l.count == 0 || l.head == kNil || l.tail == kNil) return fail("empty level kept");
                    std::uint64_t sum = 0;
                    std::uint32_t n = 0, prev = kNil;
                    for (std::uint32_t oi = l.head; oi != kNil; oi = orders_[oi].next) {
                        const Order& o = orders_[oi];
                        if (o.prev != prev) return fail("broken prev link");
                        if (o.level != li || o.price != px || o.side != s || o.locate != loc)
                            return fail("order fields disagree with level");
                        if (o.shares == 0) return fail("zero-share order kept");
                        const std::uint32_t* m = map_.find(o.ref);
                        if (m == nullptr || *m != oi) return fail("order not indexed by its ref");
                        sum += o.shares;
                        ++n;
                        prev = oi;
                        if (n > l.count) return fail("queue longer than count");
                    }
                    if (prev != l.tail) return fail("tail mismatch");
                    if (n != l.count) return fail("count mismatch");
                    if (sum != l.shares) return fail("level shares mismatch");
                    orders_seen += n;
                    return true;
                });
            };
            check_side(b.bids, 'B');
            if (err.empty()) check_side(b.asks, 'S');
            if (!err.empty()) return err;
        }
        if (orders_seen != map_.size()) return "orders in levels != orders in map";
        if (orders_seen != orders_.live()) return "orders in levels != live pool orders";
        if (levels_seen != levels_.live()) return "levels in books != live pool levels";
        return {};
    }

private:
    Result reduce(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares, Result over) {
        const std::uint32_t* slot = map_.find(ref);
        if (slot == nullptr) [[unlikely]] return Result::unknown_ref;
        const std::uint32_t oi = *slot;
        Order& o = orders_[oi];
        if (o.locate != locate) [[unlikely]] return Result::locate_mismatch;
        if (shares > o.shares) [[unlikely]] return over;
        if (shares == o.shares) {
            kill(oi);
        } else {
            o.shares -= shares;
            levels_[o.level].shares -= shares;
        }
        return Result::ok;
    }

    // Appends order oi (fields already set) at the tail of its price level.
    void link(std::uint32_t oi) {
        Order& o = orders_[oi];
        Book& b = books_[o.locate];
        auto alloc = [this, &o] {
            const std::uint32_t li = levels_.alloc();
            levels_[li] = Level{0, o.price, 0, kNil, kNil};
            return li;
        };
        const std::uint32_t li =
            o.side == 'B' ? b.bids.find_or_insert(o.price, alloc) : b.asks.find_or_insert(o.price, alloc);
        Level& l = levels_[li];
        o.level = li;
        o.next = kNil;
        o.prev = l.tail;
        if (l.tail != kNil) orders_[l.tail].next = oi; else l.head = oi;
        l.tail = oi;
        ++l.count;
        l.shares += o.shares;
    }

    // Removes order oi from its level queue, dropping the level if it empties.
    void unlink(std::uint32_t oi) {
        Order& o = orders_[oi];
        Level& l = levels_[o.level];
        if (o.prev != kNil) orders_[o.prev].next = o.next; else l.head = o.next;
        if (o.next != kNil) orders_[o.next].prev = o.prev; else l.tail = o.prev;
        l.shares -= o.shares;
        if (--l.count == 0) {
            Book& b = books_[o.locate];
            if (o.side == 'B') b.bids.erase(o.price); else b.asks.erase(o.price);
            levels_.release(o.level);
        }
    }

    void kill(std::uint32_t oi) {
        unlink(oi);
        map_.erase(orders_[oi].ref);
        orders_.release(oi);
    }

    std::vector<Book, HugePageAllocator<Book>> books_;
    BasicU64Map<Hash> map_;
    Pool<Order> orders_;
    Pool<Level> levels_;
    std::size_t peak_orders_ = 0;
};

}  // namespace itchbook
