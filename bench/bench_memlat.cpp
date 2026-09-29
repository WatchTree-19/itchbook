// bench_memlat: dependent random loads (pointer chase) over buffers of
// increasing size, with and without transparent huge pages. Gives the memory
// latency floor of the machine, which is what an order book with millions of
// live orders is ultimately bound by.
//
//   bench_memlat [--cpu N]
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "bench_common.hpp"

namespace {

double chase(std::size_t bytes, bool huge, double tpn) {
    const std::size_t n = bytes / 64;  // one pointer per cache line
    const std::size_t alloc = (n * 64 + (2u << 20) - 1) & ~std::size_t((2u << 20) - 1);
    auto* base = static_cast<char*>(std::aligned_alloc(2u << 20, alloc));
    if (!base) std::exit(2);
    if (huge) ::madvise(base, alloc, MADV_HUGEPAGE);
    else ::madvise(base, alloc, MADV_NOHUGEPAGE);
    std::vector<std::uint32_t> perm(n);
    std::iota(perm.begin(), perm.end(), 0u);
    std::mt19937_64 rng(42);
    std::shuffle(perm.begin(), perm.end(), rng);
    // Single cycle through all lines in random order.
    for (std::size_t i = 0; i < n; ++i) {
        char* from = base + std::size_t{perm[i]} * 64;
        char* to = base + std::size_t{perm[(i + 1) % n]} * 64;
        *reinterpret_cast<char**>(from) = to;
    }
    char* p = base + std::size_t{perm[0]} * 64;
    const std::size_t steps = 20'000'000;
    for (std::size_t i = 0; i < n && i < steps; ++i) p = *reinterpret_cast<char**>(p);  // warm up
    const std::uint64_t t0 = bench::tsc_start();
    for (std::size_t i = 0; i < steps; ++i) p = *reinterpret_cast<char**>(p);
    const std::uint64_t t1 = bench::tsc_stop();
    bench::do_not_optimize(p);
    std::free(base);
    return static_cast<double>(t1 - t0) / tpn / static_cast<double>(steps);
}

}  // namespace

int main(int argc, char** argv) {
    int cpu = 1;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--cpu") cpu = std::atoi(argv[i + 1]);
    bench::pin_to_cpu(cpu);
    const double tpn = bench::calibrate_tsc();
    std::printf("cpu model  %s\n", bench::cpu_model().c_str());
    std::printf("%-12s %14s %14s\n", "buffer", "4K pages ns", "THP ns");
    for (std::size_t mb : {1, 4, 16, 64, 256, 1024}) {
        const std::size_t b = mb << 20;
        std::printf("%8zu MiB %14.1f %14.1f\n", mb, chase(b, false, tpn), chase(b, true, tpn));
    }
    return 0;
}
