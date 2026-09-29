// Shared benchmark utilities: TSC timing calibrated against steady_clock,
// CPU pinning, percentile reports and a global allocation counter.
#pragma once

#include <sched.h>
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace bench {

// ------------------------------------------------------------ allocations

inline std::uint64_t g_allocs = 0;  // single-threaded benchmarks

// ------------------------------------------------------------ timing

// lfence before rdtsc keeps earlier instructions from drifting into the timed
// region; rdtscp waits for the timed work, the trailing lfence keeps later
// instructions out.
[[gnu::always_inline]] inline std::uint64_t tsc_start() noexcept {
    _mm_lfence();
    return __rdtsc();
}
[[gnu::always_inline]] inline std::uint64_t tsc_stop() noexcept {
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

// TSC ticks per nanosecond, measured against steady_clock.
inline double calibrate_tsc(int ms = 500) {
    const auto c0 = std::chrono::steady_clock::now();
    const std::uint64_t t0 = tsc_start();
    while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(ms)) {
    }
    const std::uint64_t t1 = tsc_stop();
    const auto c1 = std::chrono::steady_clock::now();
    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
    return static_cast<double>(t1 - t0) / ns;
}

// Median cost of an empty tsc_start/tsc_stop pair, in ticks.
inline std::uint64_t timer_overhead() {
    std::vector<std::uint64_t> v(200000);
    for (auto& x : v) {
        const std::uint64_t a = tsc_start();
        const std::uint64_t b = tsc_stop();
        x = b - a;
    }
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

inline bool pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof set, &set) == 0;
}

// The 1, 5 and 15 minute load averages, to show whether other work shared
// the machine during a run.
inline std::string loadavg() {
    std::ifstream f("/proc/loadavg");
    std::string a, b, c;
    f >> a >> b >> c;
    return a + " " + b + " " + c;
}

inline std::string cpu_model() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
    return "unknown";
}

template <class T>
inline void do_not_optimize(const T& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

// ------------------------------------------------------------ reporting

struct Percentiles {
    std::size_t n = 0;
    double mean = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
};

// samples in TSC ticks; converted to ns with ticks_per_ns. Sorts in place.
inline Percentiles percentiles(std::vector<std::uint32_t>& s, double ticks_per_ns) {
    Percentiles p;
    p.n = s.size();
    if (s.empty()) return p;
    std::sort(s.begin(), s.end());
    auto at = [&](double q) {
        std::size_t i = static_cast<std::size_t>(q * static_cast<double>(s.size() - 1) + 0.5);
        return static_cast<double>(s[i]) / ticks_per_ns;
    };
    double sum = 0;
    for (auto x : s) sum += x;
    p.mean = sum / static_cast<double>(s.size()) / ticks_per_ns;
    p.p50 = at(0.50);
    p.p90 = at(0.90);
    p.p99 = at(0.99);
    p.p999 = at(0.999);
    p.p9999 = at(0.9999);
    p.max = static_cast<double>(s.back()) / ticks_per_ns;
    return p;
}

inline void print_header() {
    std::printf("%-26s %12s %8s %8s %8s %8s %8s %9s %11s\n", "", "count", "mean", "p50", "p90", "p99", "p99.9",
                "p99.99", "max");
}
inline void print_row(const std::string& name, const Percentiles& p) {
    std::printf("%-26s %12zu %8.1f %8.1f %8.1f %8.1f %8.1f %9.1f %11.1f\n", name.c_str(), p.n, p.mean, p.p50, p.p90,
                p.p99, p.p999, p.p9999, p.max);
}

}  // namespace bench

// Count every allocation made through operator new in the benchmark binary.
// NOLINTBEGIN
void* operator new(std::size_t n) {
    ++bench::g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    ++bench::g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t al) {
    ++bench::g_allocs;
    const std::size_t a = static_cast<std::size_t>(al);
    if (void* p = std::aligned_alloc(a, (n + a - 1) / a * a)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
// NOLINTEND
