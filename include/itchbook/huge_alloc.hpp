// Allocator that asks the kernel for transparent huge pages on large buffers.
//
// The order map and the order pool are hundreds of megabytes and are accessed
// at random, so with 4 KiB pages nearly every lookup also misses the TLB.
// Buffers of 2 MiB or more are allocated 2 MiB aligned and marked with
// madvise(MADV_HUGEPAGE), which is honoured when
// /sys/kernel/mm/transparent_hugepage/enabled is "always" or "madvise".
// Smaller buffers use the ordinary allocator. Define ITCHBOOK_NO_HUGEPAGES to
// turn the madvise off (used to measure the difference).
#pragma once

#include <sys/mman.h>

#include <cstddef>
#include <memory>
#include <new>

namespace itchbook {

template <class T>
struct HugePageAllocator {
    using value_type = T;
    static constexpr std::size_t kHuge = std::size_t{2} << 20;

    HugePageAllocator() noexcept = default;
    template <class U>
    HugePageAllocator(const HugePageAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        if (bytes < kHuge) return std::allocator<T>{}.allocate(n);
        const std::size_t rounded = (bytes + kHuge - 1) & ~(kHuge - 1);
        void* p = ::operator new(rounded, std::align_val_t{kHuge});
#if defined(MADV_HUGEPAGE) && !defined(ITCHBOOK_NO_HUGEPAGES)
        ::madvise(p, rounded, MADV_HUGEPAGE);  // advisory: failure just means 4 KiB pages
#endif
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        const std::size_t bytes = n * sizeof(T);
        if (bytes < kHuge) return std::allocator<T>{}.deallocate(p, n);
        ::operator delete(p, std::align_val_t{kHuge});
    }

    template <class U>
    bool operator==(const HugePageAllocator<U>&) const noexcept { return true; }
};

}  // namespace itchbook
