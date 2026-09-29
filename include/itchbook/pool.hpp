// Object pool and the price level record shared by the ITCH book and the
// matching engine.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "itchbook/huge_alloc.hpp"

namespace itchbook {

struct Level {
    std::uint64_t shares;  // sum of displayed shares of the orders queued here
    std::uint32_t price;
    std::uint32_t count;   // number of orders queued here
    std::uint32_t head;    // oldest order (first to execute)
    std::uint32_t tail;    // newest order
};
static_assert(sizeof(Level) == 24);

// Vector-backed object pool with a free list. Growth past the reserved
// capacity reallocates; pools are sized up front so that does not happen on
// the measured path, and grow_count() reports it if it ever does.
template <class T>
class Pool {
public:
    explicit Pool(std::size_t reserve) {
        items_.reserve(reserve);
        free_.reserve(reserve);
    }
    [[nodiscard]] std::uint32_t alloc() {
        if (!free_.empty()) {
            const std::uint32_t i = free_.back();
            free_.pop_back();
            ++live_;
            return i;
        }
        if (items_.size() == items_.capacity()) [[unlikely]] ++grows_;
        items_.emplace_back();
        ++live_;
        return static_cast<std::uint32_t>(items_.size() - 1);
    }
    void release(std::uint32_t i) {
        if (free_.size() == free_.capacity()) [[unlikely]] ++grows_;
        free_.push_back(i);
        --live_;
    }
    [[nodiscard]] T& operator[](std::uint32_t i) noexcept { return items_[i]; }
    [[nodiscard]] const T& operator[](std::uint32_t i) const noexcept { return items_[i]; }
    [[nodiscard]] std::size_t live() const noexcept { return live_; }
    [[nodiscard]] std::size_t grow_count() const noexcept { return grows_; }
    void clear() noexcept {
        items_.clear();
        free_.clear();
        live_ = 0;
    }

private:
    std::vector<T, HugePageAllocator<T>> items_;
    std::vector<std::uint32_t, HugePageAllocator<std::uint32_t>> free_;
    std::size_t live_ = 0;
    std::size_t grows_ = 0;
};

}  // namespace itchbook
