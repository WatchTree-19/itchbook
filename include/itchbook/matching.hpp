// Price-time priority matching engine (the venue side, not a feed handler).
//
// Order types: limit and market. Time in force: Day (the unfilled remainder of
// a limit order rests) and IOC (the remainder is cancelled). A market order
// never rests; whatever it cannot fill is cancelled.
//
// Matching: an incoming order trades against the opposite side best price
// first and, within a price, oldest order first. Every fill prints at the
// resting (maker) order's price.
//
// Cancel/replace (replace(id, price, qty), qty is the new open quantity):
//   * same price and qty <= current open qty: amended in place, keeps its
//     queue position;
//   * same price and a larger qty, or a different price: the order leaves the
//     book and re-enters as a new Day limit order with the same id, so it goes
//     to the back of the queue, and at a new price it can trade immediately.
//
// Rejects (the book is unchanged): qty 0, a limit price outside the configured
// [min_price, max_price] band, the reserved id 2^64 - 1, a new order whose id
// is live, cancel or replace of an id that is not live. Checked in that order.
// Replace checks: unknown id, then qty 0, then price band.
//
// Events go to a Listener through optional member functions (checked with
// requires-expressions, so an empty listener costs nothing):
//   on_trade(const TradeEvent&)
//   on_level(Side, price, total_qty, order_count)  after any change to a level;
//                                                  total_qty 0 means it is gone
//   on_rest(id, Side, price, qty)                  an order joined the book
//   on_amend(id, price, qty)                       in-place reduce, priority kept
//   on_cancel(id, qty, CancelReason)
//   on_reject(id, RejectReason)
// Event order per operation: for each fill, on_trade then on_level of the
// maker's level; then either on_rest + on_level or on_cancel for the remainder.
//
// The engine never allocates on the hot path when the pools, the id map and
// (for VectorLevels) the level vectors are within their reserved sizes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "itchbook/hash_map.hpp"
#include "itchbook/levels.hpp"
#include "itchbook/pool.hpp"

namespace itchbook {

enum class Side : std::uint8_t { buy = 0, sell = 1 };
enum class OrderType : std::uint8_t { limit = 0, market = 1 };
enum class TimeInForce : std::uint8_t { day = 0, ioc = 1 };
enum class RejectReason : std::uint8_t { duplicate_id, unknown_id, bad_quantity, bad_price, reserved_id };
enum class CancelReason : std::uint8_t { user, ioc_remainder, market_remainder };

struct NewOrder {
    std::uint64_t id;
    Side side;
    OrderType type;
    TimeInForce tif;
    std::uint32_t price;  // ticks; ignored for market orders
    std::uint32_t qty;
};

struct TradeEvent {
    std::uint64_t taker_id;
    std::uint64_t maker_id;
    std::uint32_t price;
    std::uint32_t qty;
    Side taker_side;
};

struct MatchOrder {
    std::uint64_t id;
    std::uint32_t qty;
    std::uint32_t price;
    std::uint32_t prev;
    std::uint32_t next;
    std::uint32_t level;
    Side side;
};

struct EngineConfig {
    std::uint32_t min_price = 1;
    std::uint32_t max_price = 1u << 20;
    std::size_t expected_orders = std::size_t{1} << 20;
    std::size_t expected_levels = std::size_t{1} << 16;
};

template <class Listener, template <bool> class LevelsT = VectorLevels>
class MatchingEngine {
public:
    MatchingEngine(Listener& listener, const EngineConfig& cfg = {})
        : l_(listener),
          cfg_(cfg),
          bids_(LevelConfig{cfg.min_price, cfg.max_price}),
          asks_(LevelConfig{cfg.min_price, cfg.max_price}),
          map_(cfg.expected_orders),
          orders_(cfg.expected_orders),
          levels_(cfg.expected_levels) {}

    void submit(const NewOrder& o) {
        if (o.qty == 0) [[unlikely]] return reject(o.id, RejectReason::bad_quantity);
        if (o.type == OrderType::limit && !price_ok(o.price)) [[unlikely]]
            return reject(o.id, RejectReason::bad_price);
        if (o.id == U64Map::kEmpty) [[unlikely]] return reject(o.id, RejectReason::reserved_id);
        if (map_.find(o.id) != nullptr) [[unlikely]] return reject(o.id, RejectReason::duplicate_id);
        execute_incoming(o.id, o.side, o.type, o.tif, o.price, o.qty);
    }

    void cancel(std::uint64_t id) {
        const std::uint32_t* slot = map_.find(id);
        if (slot == nullptr) [[unlikely]] return reject(id, RejectReason::unknown_id);
        const std::uint32_t oi = *slot;
        const MatchOrder o = orders_[oi];
        remove_resting(oi);
        if constexpr (requires { l_.on_cancel(id, o.qty, CancelReason::user); })
            l_.on_cancel(id, o.qty, CancelReason::user);
        emit_level(o.side, o.price);
    }

    void replace(std::uint64_t id, std::uint32_t new_price, std::uint32_t new_qty) {
        const std::uint32_t* slot = map_.find(id);
        if (slot == nullptr) [[unlikely]] return reject(id, RejectReason::unknown_id);
        if (new_qty == 0) [[unlikely]] return reject(id, RejectReason::bad_quantity);
        if (!price_ok(new_price)) [[unlikely]] return reject(id, RejectReason::bad_price);
        const std::uint32_t oi = *slot;
        MatchOrder& o = orders_[oi];
        if (new_price == o.price && new_qty <= o.qty) {
            // Reduce (or no change) at the same price: keep queue position.
            levels_[o.level].shares -= (o.qty - new_qty);
            o.qty = new_qty;
            if constexpr (requires { l_.on_amend(id, new_price, new_qty); }) l_.on_amend(id, new_price, new_qty);
            emit_level(o.side, o.price);
            return;
        }
        // Loses priority: leave the book, re-enter as a new Day limit order.
        const Side side = o.side;
        const std::uint32_t old_price = o.price;
        remove_resting(oi);
        emit_level(side, old_price);
        execute_incoming(id, side, OrderType::limit, TimeInForce::day, new_price, new_qty);
    }

    // Empties the book but keeps all reserved capacity.
    void clear() {
        bids_.clear();
        asks_.clear();
        map_.clear();
        orders_.clear();
        levels_.clear();
    }

    // ------------------------------------------------------------ queries

    [[nodiscard]] std::uint32_t best_bid() const noexcept { return bids_.best_price(); }
    [[nodiscard]] std::uint32_t best_ask() const noexcept { return asks_.best_price(); }
    [[nodiscard]] std::size_t live_orders() const noexcept { return map_.size(); }
    [[nodiscard]] std::size_t map_rehashes() const noexcept { return map_.rehash_count(); }
    [[nodiscard]] std::size_t pool_grows() const noexcept { return orders_.grow_count() + levels_.grow_count(); }

    [[nodiscard]] const MatchOrder* find(std::uint64_t id) const noexcept {
        const std::uint32_t* slot = map_.find(id);
        return slot ? &orders_[*slot] : nullptr;
    }

    // Levels of one side best first; f(const Level&) returns false to stop.
    template <class F>
    void for_each_level(Side side, F&& f) const {
        auto visit = [&](std::uint32_t, std::uint32_t li) { return f(levels_[li]); };
        if (side == Side::buy) bids_.for_each(visit); else asks_.for_each(visit);
    }
    template <class F>
    void for_each_order(const Level& l, F&& f) const {
        for (std::uint32_t oi = l.head; oi != kNil; oi = orders_[oi].next) f(orders_[oi]);
    }

    // Structural consistency check; empty string when consistent.
    [[nodiscard]] std::string audit() const {
        std::string err;
        std::size_t seen = 0;
        auto side_check = [&](const auto& levels, Side s) {
            std::uint32_t prev_px = 0;
            bool first = true;
            levels.for_each([&](std::uint32_t px, std::uint32_t li) {
                const Level& l = levels_[li];
                if (l.price != px) { err = "level price mismatch"; return false; }
                if (!first && ((s == Side::buy) ? px >= prev_px : px <= prev_px)) {
                    err = "levels out of order";
                    return false;
                }
                first = false;
                prev_px = px;
                std::uint64_t sum = 0;
                std::uint32_t n = 0, prev = kNil;
                for (std::uint32_t oi = l.head; oi != kNil; oi = orders_[oi].next) {
                    const MatchOrder& o = orders_[oi];
                    if (o.prev != prev || o.level != li || o.price != px || o.side != s || o.qty == 0) {
                        err = "order inconsistent with level";
                        return false;
                    }
                    const std::uint32_t* m = map_.find(o.id);
                    if (m == nullptr || *m != oi) { err = "order not indexed"; return false; }
                    sum += o.qty;
                    prev = oi;
                    if (++n > l.count) { err = "queue longer than count"; return false; }
                }
                if (n == 0 || n != l.count || sum != l.shares || prev != l.tail) {
                    err = "level aggregate mismatch";
                    return false;
                }
                seen += n;
                return true;
            });
        };
        side_check(bids_, Side::buy);
        if (err.empty()) side_check(asks_, Side::sell);
        if (err.empty() && seen != map_.size()) err = "orders in book != orders in map";
        if (err.empty() && !bids_.empty() && !asks_.empty() && bids_.best_price() >= asks_.best_price())
            err = "book is crossed";
        return err;
    }

private:
    [[nodiscard]] bool price_ok(std::uint32_t px) const noexcept {
        return px >= cfg_.min_price && px <= cfg_.max_price;
    }

    void reject(std::uint64_t id, RejectReason r) {
        if constexpr (requires { l_.on_reject(id, r); }) l_.on_reject(id, r);
    }

    void emit_level(Side side, std::uint32_t price) {
        if constexpr (requires { l_.on_level(side, price, std::uint64_t{}, std::uint32_t{}); }) {
            const std::uint32_t li = side == Side::buy ? bids_.find(price) : asks_.find(price);
            if (li == kNil) l_.on_level(side, price, std::uint64_t{0}, std::uint32_t{0});
            else l_.on_level(side, price, levels_[li].shares, levels_[li].count);
        }
    }

    void execute_incoming(std::uint64_t id, Side side, OrderType type, TimeInForce tif, std::uint32_t price,
                          std::uint32_t qty) {
        if (side == Side::buy) qty = match<false>(asks_, id, side, type, price, qty);
        else qty = match<true>(bids_, id, side, type, price, qty);
        if (qty == 0) return;
        if (type == OrderType::market) {
            if constexpr (requires { l_.on_cancel(id, qty, CancelReason::market_remainder); })
                l_.on_cancel(id, qty, CancelReason::market_remainder);
            return;
        }
        if (tif == TimeInForce::ioc) {
            if constexpr (requires { l_.on_cancel(id, qty, CancelReason::ioc_remainder); })
                l_.on_cancel(id, qty, CancelReason::ioc_remainder);
            return;
        }
        rest(id, side, price, qty);
    }

    // Matches against the opposite side (MakerBid tells which side that is).
    // Returns the unfilled quantity.
    template <bool MakerBid, class Side_>
    std::uint32_t match(Side_& book, std::uint64_t taker, Side taker_side, OrderType type, std::uint32_t limit,
                        std::uint32_t qty) {
        const Side maker_side = MakerBid ? Side::buy : Side::sell;
        while (qty > 0) {
            const std::uint32_t li = book.best();
            if (li == kNil) break;
            Level& lv = levels_[li];
            const std::uint32_t px = lv.price;
            // A buy crosses asks priced <= limit; a sell crosses bids >= limit.
            if (type == OrderType::limit && better<MakerBid>(limit, px)) break;
            while (qty > 0 && lv.head != kNil) {
                const std::uint32_t mi = lv.head;
                MatchOrder& m = orders_[mi];
                const std::uint32_t fill = qty < m.qty ? qty : m.qty;
                qty -= fill;
                m.qty -= fill;
                lv.shares -= fill;
                const std::uint64_t maker_id = m.id;
                const bool maker_done = m.qty == 0;
                const std::uint32_t count_after = maker_done ? lv.count - 1 : lv.count;
                const std::uint64_t shares_after = lv.shares;
                if constexpr (requires { l_.on_trade(TradeEvent{}); })
                    l_.on_trade(TradeEvent{taker, maker_id, px, fill, taker_side});
                if (maker_done) remove_resting(mi);  // may free the level
                if constexpr (requires { l_.on_level(maker_side, px, std::uint64_t{}, std::uint32_t{}); })
                    l_.on_level(maker_side, px, count_after == 0 ? std::uint64_t{0} : shares_after, count_after);
                if (count_after == 0) break;  // level is gone, lv is dangling
            }
        }
        return qty;
    }

    void rest(std::uint64_t id, Side side, std::uint32_t price, std::uint32_t qty) {
        const std::uint32_t oi = orders_.alloc();
        map_.insert(id, oi);
        MatchOrder& o = orders_[oi];
        o.id = id;
        o.qty = qty;
        o.price = price;
        o.side = side;
        auto alloc = [this, price] {
            const std::uint32_t li = levels_.alloc();
            levels_[li] = Level{0, price, 0, kNil, kNil};
            return li;
        };
        const std::uint32_t li =
            side == Side::buy ? bids_.find_or_insert(price, alloc) : asks_.find_or_insert(price, alloc);
        Level& l = levels_[li];
        o.level = li;
        o.next = kNil;
        o.prev = l.tail;
        if (l.tail != kNil) orders_[l.tail].next = oi; else l.head = oi;
        l.tail = oi;
        ++l.count;
        l.shares += qty;
        if constexpr (requires { l_.on_rest(id, side, price, qty); }) l_.on_rest(id, side, price, qty);
        if constexpr (requires { l_.on_level(side, price, std::uint64_t{}, std::uint32_t{}); })
            l_.on_level(side, price, l.shares, l.count);
    }

    // Unlinks a resting order, drops its level if empty, frees it.
    void remove_resting(std::uint32_t oi) {
        MatchOrder& o = orders_[oi];
        Level& l = levels_[o.level];
        if (o.prev != kNil) orders_[o.prev].next = o.next; else l.head = o.next;
        if (o.next != kNil) orders_[o.next].prev = o.prev; else l.tail = o.prev;
        l.shares -= o.qty;
        if (--l.count == 0) {
            if (o.side == Side::buy) bids_.erase(o.price); else asks_.erase(o.price);
            levels_.release(o.level);
        }
        map_.erase(o.id);
        orders_.release(oi);
    }

    Listener& l_;
    EngineConfig cfg_;
    LevelsT<true> bids_;
    LevelsT<false> asks_;
    U64Map map_;
    Pool<MatchOrder> orders_;
    Pool<Level> levels_;
};

}  // namespace itchbook
