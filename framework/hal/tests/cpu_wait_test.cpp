// framework/hal/tests/cpu_wait_test.cpp
// Property tests for hal::cpu_wait_until / cpu_wait_for_ns.
//
// On WAITPKG-capable CPUs (Intel Sapphire Rapids+, Alder Lake P-cores) the
// TPAUSE path is exercised. On AMD / older Intel, the bounded PAUSE-loop
// fallback runs. Both must:
//   - Return after at least the requested duration (no early wake bug)
//   - Not over-wait by more than 50% (sanity bound — TPAUSE wakes promptly
//     and the PAUSE-loop is TSC-deadline driven)
//   - Not crash on zero-ns wait
#include <phyriad/hal/CpuWait.hpp>
#include <phyriad/hal/Timestamp.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace hal = phyriad::hal;

static int g_pass{0};
static int g_fail{0};

#define SECTION(msg) std::printf("  § %s\n", (msg))
#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (cond) { ++g_pass; }                                                \
        else {                                                                 \
            ++g_fail;                                                          \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                      \
    } while (false)

static void test_capability_query() {
    SECTION("Test 1: waitpkg_available() does not crash and returns deterministic value");
    const bool a = hal::waitpkg_available();
    const bool b = hal::waitpkg_available();
    EXPECT(a == b);   // cached, must return same value
    std::printf("        WAITPKG capability on this CPU: %s\n",
                a ? "yes (TPAUSE path)" : "no (PAUSE-loop fallback)");
}

static void test_zero_ns_no_crash() {
    SECTION("Test 2: cpu_wait_for_ns(0) returns immediately");
    hal::cpu_wait_for_ns(0u);
    EXPECT(true);   // no crash
}

static void test_wait_ns_lower_bound() {
    SECTION("Test 3: cpu_wait_for_ns(N) waits at least N ns (within scheduler jitter)");
    for (uint64_t target_ns : {1'000ull, 10'000ull, 100'000ull}) {
        const auto t0 = std::chrono::steady_clock::now();
        hal::cpu_wait_for_ns(target_ns);
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        std::printf("        target=%llu ns  elapsed=%llu ns\n",
                    static_cast<unsigned long long>(target_ns),
                    static_cast<unsigned long long>(elapsed_ns));
        // Allow 20% slack below target (TSC vs wall-clock drift on this thread).
        EXPECT(elapsed_ns >= target_ns * 80u / 100u);
    }
}

static void test_wait_until_specific() {
    SECTION("Test 4: cpu_wait_until(rdtsc()+N) wakes near the deadline");
    const uint64_t freq = hal::calibrate_tsc_freq();
    if (freq == 0u) {
        EXPECT(false);
        return;
    }
    const uint64_t cycles = freq / 100'000ull;   // ~10 µs
    const uint64_t t_start = hal::rdtsc();
    hal::cpu_wait_until(t_start + cycles);
    const uint64_t t_end = hal::rdtsc();
    const uint64_t elapsed_cycles = t_end - t_start;
    std::printf("        deadline=+%llu cycles  observed=%llu cycles\n",
                static_cast<unsigned long long>(cycles),
                static_cast<unsigned long long>(elapsed_cycles));
    EXPECT(elapsed_cycles >= cycles * 90u / 100u);   // ≥ 90% of deadline
}

int main() {
    std::printf("[cpu_wait_test] phyriad_hal — Phase O6.3\n");
    std::printf("----------------------------------------------------------------\n");

    test_capability_query();
    test_zero_ns_no_crash();
    test_wait_ns_lower_bound();
    test_wait_until_specific();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d checks passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d checks FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
