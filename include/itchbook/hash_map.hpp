// Open-addressing hash map from a 64-bit key to a 32-bit value.
//
// The key 2^64 - 1 (kEmpty) marks free slots and cannot be stored: insert()
// refuses it and find() never finds it. Callers that take keys from outside
// (the ITCH book, the matching engine) reject it explicitly.
//
// Used for order reference -> order pool index lookups. Linear probing with
// backward-shift deletion (no tombstones, so probe lengths do not degrade over
// a long session). Capacity is a power of two and is allocated up front; the
// only allocation after construction is an explicit rehash when the load factor
// would exceed 1/2, which is counted in rehash_count() so a replay can prove it
// never happened on the measured path.
//
// Two slot functions:
//   fibonacci  multiply by 2^64/phi and keep the high bits. Spreads any key
//              pattern; use it when keys are chosen by someone else (client
//              order ids in the matching engine).
//   identity   key & mask. Nasdaq assigns order references in increasing
//              order, so consecutive orders land in consecutive slots and the
//              recently added (most active) orders share cache lines. Only
//              safe for keys that are not adversarial, and it degrades if keys
//              advance in a fixed stride. Measured on the ITCH replay it was
//              no faster than fibonacci, so nothing uses it by default.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "itchbook/huge_alloc.hpp"

namespace itchbook {

enum class MapHash { fibonacci, identity };

template <MapHash Hash>
class BasicU64Map {
public:
    static constexpr std::uint64_t kEmpty = ~std::uint64_t{0};

    explicit BasicU64Map(std::size_t min_capacity = 1024) { init(round_up(min_capacity * 2)); }

    // Returns a pointer to the mapped value, or nullptr.
    [[nodiscard]] std::uint32_t* find(std::uint64_t key) noexcept {
        if (key == kEmpty) [[unlikely]] return nullptr;
        std::size_t i = slot_of(key);
        for (;;) {
            Slot& s = slots_[i];
            if (s.key == key) return &s.value;
            if (s.key == kEmpty) return nullptr;
            i = (i + 1) & mask_;
        }
    }
    [[nodiscard]] const std::uint32_t* find(std::uint64_t key) const noexcept {
        return const_cast<BasicU64Map*>(this)->find(key);
    }

    // Hint: start loading the cache line of key's home slot.
    void prefetch(std::uint64_t key) const noexcept { __builtin_prefetch(&slots_[slot_of(key)]); }

    // Inserts key -> value. Returns false (and changes nothing) if the key
    // exists or is the reserved kEmpty.
    bool insert(std::uint64_t key, std::uint32_t value) {
        if (key == kEmpty) [[unlikely]] return false;
        if ((size_ + 1) * 2 > slots_.size()) [[unlikely]]
            rehash(slots_.size() * 2);
        std::size_t i = slot_of(key);
        for (;;) {
            Slot& s = slots_[i];
            if (s.key == kEmpty) {
                s.key = key;
                s.value = value;
                ++size_;
                return true;
            }
            if (s.key == key) return false;
            i = (i + 1) & mask_;
        }
    }

    // Removes key. Returns false if it was not present.
    bool erase(std::uint64_t key) noexcept {
        if (key == kEmpty) [[unlikely]] return false;
        std::size_t i = slot_of(key);
        for (;;) {
            if (slots_[i].key == key) break;
            if (slots_[i].key == kEmpty) return false;
            i = (i + 1) & mask_;
        }
        // Backward-shift deletion: pull later members of the probe run into
        // the hole whenever their home slot does not lie cyclically in (i, j].
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].key == kEmpty) break;
            const std::size_t home = slot_of(slots_[j].key);
            const bool home_in_range = (i <= j) ? (home > i && home <= j) : (home > i || home <= j);
            if (!home_in_range) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].key = kEmpty;
        --size_;
        return true;
    }

    void clear() noexcept {
        for (auto& s : slots_) s.key = kEmpty;
        size_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t rehash_count() const noexcept { return rehashes_; }

    template <class F>
    void for_each(F&& f) const {
        for (const auto& s : slots_)
            if (s.key != kEmpty) f(s.key, s.value);
    }

private:
    struct Slot {
        std::uint64_t key;
        std::uint32_t value;
    };

    static std::size_t round_up(std::size_t n) {
        std::size_t c = 16;
        while (c < n) c <<= 1;
        return c;
    }

    void init(std::size_t cap) {
        slots_.assign(cap, Slot{kEmpty, 0});
        mask_ = cap - 1;
        shift_ = 64;
        for (std::size_t c = cap; c > 1; c >>= 1) --shift_;
        size_ = 0;
    }

    [[nodiscard]] std::size_t slot_of(std::uint64_t key) const noexcept {
        if constexpr (Hash == MapHash::identity) return static_cast<std::size_t>(key) & mask_;
        else return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    [[gnu::noinline]] void rehash(std::size_t new_cap) {
        std::vector<Slot, HugePageAllocator<Slot>> old;
        old.swap(slots_);
        init(new_cap);
        for (const auto& s : old)
            if (s.key != kEmpty) insert(s.key, s.value);
        ++rehashes_;
    }

    std::vector<Slot, HugePageAllocator<Slot>> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
    std::size_t size_ = 0;
    std::size_t rehashes_ = 0;
};

using U64Map = BasicU64Map<MapHash::fibonacci>;

}  // namespace itchbook
