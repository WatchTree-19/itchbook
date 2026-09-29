// Price level storage for one side of a book.
//
// A side container maps a price (u32 ticks) to the index of a Level held in a
// pool owned by the book. The Level itself (aggregate shares, order count and
// the FIFO queue of orders) is the same for every container; only the
// price -> level lookup and the best-price bookkeeping differ. Three
// interchangeable implementations are provided so they can be benchmarked
// against identical workloads:
//
//   MapLevels      std::map keyed by price (red-black tree, one allocation per
//                  new level, O(log n) everything). The textbook baseline.
//   VectorLevels   flat vector of (price, level) sorted worst -> best, so the
//                  best price is at the back. Lookups scan linearly from the
//                  back for LinearScan entries (most book activity is at or near
//                  the touch) and fall back to binary search. Inserting or
//                  erasing near the touch moves only the few better entries.
//                  LinearScan = 0 gives a pure binary-search variant.
//   LadderLevels   dense array indexed by (price - min_price) plus an occupancy
//                  bitmap for finding the next best price. O(1) lookup but needs
//                  a bounded price range, so it only suits the matching engine
//                  where the venue defines the band.
//
// All containers expose the same interface:
//   find(px) -> level index or kNil
//   find_or_insert(px, alloc) -> level index, calling alloc() only when new
//   erase(px), best() -> level index or kNil, best_price(), size(), empty(),
//   for_each(f) in priority order (best first) while f(px, idx) returns true,
//   clear().
#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace itchbook {

inline constexpr std::uint32_t kNil = 0xFFFFFFFFu;

// Price range hint. MapLevels and VectorLevels ignore it; LadderLevels needs it.
struct LevelConfig {
    std::uint32_t min_price = 1;
    std::uint32_t max_price = 1u << 20;
};

// True if price a has strictly higher priority than price b on this side.
template <bool Bid>
[[nodiscard]] constexpr bool better(std::uint32_t a, std::uint32_t b) noexcept {
    if constexpr (Bid) return a > b; else return a < b;
}

// ---------------------------------------------------------------- map

template <bool Bid>
class MapLevels {
    struct Cmp {
        bool operator()(std::uint32_t a, std::uint32_t b) const noexcept { return better<Bid>(a, b); }
    };

public:
    explicit MapLevels(const LevelConfig& = {}) {}

    [[nodiscard]] std::uint32_t find(std::uint32_t px) const noexcept {
        auto it = m_.find(px);
        return it == m_.end() ? kNil : it->second;
    }
    template <class Alloc>
    std::uint32_t find_or_insert(std::uint32_t px, Alloc&& alloc) {
        auto it = m_.lower_bound(px);
        if (it != m_.end() && it->first == px) return it->second;
        const std::uint32_t idx = alloc();
        m_.emplace_hint(it, px, idx);
        return idx;
    }
    void erase(std::uint32_t px) { m_.erase(px); }
    [[nodiscard]] std::uint32_t best() const noexcept { return m_.empty() ? kNil : m_.begin()->second; }
    [[nodiscard]] std::uint32_t best_price() const noexcept { return m_.empty() ? 0 : m_.begin()->first; }
    [[nodiscard]] std::size_t size() const noexcept { return m_.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_.empty(); }
    template <class F>
    void for_each(F&& f) const {
        for (const auto& [px, idx] : m_)
            if (!f(px, idx)) return;
    }
    void clear() { m_.clear(); }

private:
    std::map<std::uint32_t, std::uint32_t, Cmp> m_;
};

// ---------------------------------------------------------------- vector

template <bool Bid, unsigned LinearScan>
class SortedVectorLevels {
    struct Entry {
        std::uint32_t price;
        std::uint32_t idx;
    };

public:
    explicit SortedVectorLevels(const LevelConfig& = {}) {}

    [[nodiscard]] std::uint32_t find(std::uint32_t px) const noexcept {
        const std::size_t pos = position(px);
        return (pos < v_.size() && v_[pos].price == px) ? v_[pos].idx : kNil;
    }
    template <class Alloc>
    std::uint32_t find_or_insert(std::uint32_t px, Alloc&& alloc) {
        const std::size_t pos = position(px);
        if (pos < v_.size() && v_[pos].price == px) return v_[pos].idx;
        const std::uint32_t idx = alloc();
        v_.insert(v_.begin() + static_cast<std::ptrdiff_t>(pos), Entry{px, idx});
        return idx;
    }
    void erase(std::uint32_t px) noexcept {
        const std::size_t pos = position(px);
        assert(pos < v_.size() && v_[pos].price == px);
        v_.erase(v_.begin() + static_cast<std::ptrdiff_t>(pos));
    }
    [[nodiscard]] std::uint32_t best() const noexcept { return v_.empty() ? kNil : v_.back().idx; }
    [[nodiscard]] std::uint32_t best_price() const noexcept { return v_.empty() ? 0 : v_.back().price; }
    [[nodiscard]] std::size_t size() const noexcept { return v_.size(); }
    [[nodiscard]] bool empty() const noexcept { return v_.empty(); }
    template <class F>
    void for_each(F&& f) const {
        for (std::size_t i = v_.size(); i-- > 0;)
            if (!f(v_[i].price, v_[i].idx)) return;
    }
    void clear() noexcept { v_.clear(); }

private:
    // Index of px if present, otherwise the index where it would be inserted
    // (the first entry that is not worse than px).
    [[nodiscard]] std::size_t position(std::uint32_t px) const noexcept {
        std::size_t hi = v_.size();  // entries in [hi, size) are all better than px
        for (unsigned step = 0; step < LinearScan; ++step) {
            if (hi == 0) return 0;
            const std::uint32_t p = v_[hi - 1].price;
            if (p == px) return hi - 1;
            if (better<Bid>(px, p)) return hi;
            --hi;
        }
        // Binary search in [0, hi): the first entry not worse than px.
        std::size_t lo = 0;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (better<Bid>(px, v_[mid].price)) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    std::vector<Entry> v_;
};

// One-parameter aliases so the containers fit template <bool> class slots.
template <bool Bid>
using VectorLevels = SortedVectorLevels<Bid, 8>;
template <bool Bid>
using VectorBinaryLevels = SortedVectorLevels<Bid, 0>;

// ---------------------------------------------------------------- ladder

template <bool Bid>
class LadderLevels {
public:
    explicit LadderLevels(const LevelConfig& cfg = {})
        : min_(cfg.min_price),
          n_(static_cast<std::size_t>(cfg.max_price - cfg.min_price) + 1),
          slot_(n_, kNil),
          bits_((n_ + 63) / 64, 0) {
        assert(cfg.max_price >= cfg.min_price);
    }

    [[nodiscard]] bool in_range(std::uint32_t px) const noexcept {
        return px >= min_ && static_cast<std::size_t>(px - min_) < n_;
    }
    [[nodiscard]] std::uint32_t find(std::uint32_t px) const noexcept {
        return in_range(px) ? slot_[px - min_] : kNil;
    }
    template <class Alloc>
    std::uint32_t find_or_insert(std::uint32_t px, Alloc&& alloc) {
        assert(in_range(px));
        const std::size_t o = px - min_;
        if (slot_[o] != kNil) return slot_[o];
        const std::uint32_t idx = alloc();
        slot_[o] = idx;
        bits_[o >> 6] |= std::uint64_t{1} << (o & 63);
        ++size_;
        if (size_ == 1 || better<Bid>(px, best_px_)) best_px_ = px;
        return idx;
    }
    void erase(std::uint32_t px) noexcept {
        const std::size_t o = px - min_;
        assert(in_range(px) && slot_[o] != kNil);
        slot_[o] = kNil;
        bits_[o >> 6] &= ~(std::uint64_t{1} << (o & 63));
        --size_;
        if (px == best_px_) best_px_ = size_ == 0 ? 0 : next_from(o);
    }
    [[nodiscard]] std::uint32_t best() const noexcept { return size_ == 0 ? kNil : slot_[best_px_ - min_]; }
    [[nodiscard]] std::uint32_t best_price() const noexcept { return size_ == 0 ? 0 : best_px_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    template <class F>
    void for_each(F&& f) const {
        if (size_ == 0) return;
        std::size_t o = best_px_ - min_;
        for (std::size_t seen = 0; seen < size_; ++seen) {
            if (!f(static_cast<std::uint32_t>(o + min_), slot_[o])) return;
            if (seen + 1 < size_) o = next_from(o) - min_;
        }
    }
    void clear() noexcept {
        std::fill(slot_.begin(), slot_.end(), kNil);
        std::fill(bits_.begin(), bits_.end(), 0);
        size_ = 0;
        best_px_ = 0;
    }

private:
    // Price of the next occupied slot strictly worse than offset o. Bids scan
    // down, asks scan up. Precondition: such a slot exists.
    [[nodiscard]] std::uint32_t next_from(std::size_t o) const noexcept {
        if constexpr (Bid) {
            if (o == 0) return 0;
            std::size_t w = (o - 1) >> 6;
            std::uint64_t word = bits_[w] & (~std::uint64_t{0} >> (63 - ((o - 1) & 63)));
            while (word == 0) {
                assert(w > 0);
                word = bits_[--w];
            }
            return static_cast<std::uint32_t>((w << 6) + 63 - static_cast<std::size_t>(std::countl_zero(word)) + min_);
        } else {
            std::size_t w = (o + 1) >> 6;
            if (w >= bits_.size()) return 0;
            std::uint64_t word = bits_[w] & (~std::uint64_t{0} << ((o + 1) & 63));
            while (word == 0) {
                assert(w + 1 < bits_.size());
                word = bits_[++w];
            }
            return static_cast<std::uint32_t>((w << 6) + static_cast<std::size_t>(std::countr_zero(word)) + min_);
        }
    }

    std::uint32_t min_;
    std::size_t n_;
    std::vector<std::uint32_t> slot_;
    std::vector<std::uint64_t> bits_;
    std::size_t size_ = 0;
    std::uint32_t best_px_ = 0;
};

}  // namespace itchbook
