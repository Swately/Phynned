// framework/hal/tests/hal_types_test.cpp
// Test suite for the phyriad_hal INTERFACE pillar.
//
// Tests:
//   1. kDestructivePad es potencia de 2 y >= 64.
//   2. rdtsc() es monótono (dos lecturas consecutivas no decrece).
//   3. calibrate_tsc_freq() retorna valor razonable (> 100 MHz, < 10 GHz).
//   4. tsc_to_ns() produce valor razonable para un sleep de 10 ms.
//   5. seq_store_release / seq_load_acquire round-trip.
//   6. stat_fetch_add_relaxed acumula correctamente.
//   7. prefetch() no crashea (instrucción ignorada si no hay soporte).
//   8. simd_caps() no lanza (smoke test de detección runtime).
//

#include <phyriad/hal/HAL.hpp>

#include <cstdio>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <thread>

namespace hal = phyriad::hal;

// ─────────────────────────────────────────────────────────────────────────────
// Micro-test framework
// ─────────────────────────────────────────────────────────────────────────────
static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define EXPECT(cond)                                                        \
    do {                                                                    \
        ++g_tests_run;                                                      \
        if (!(cond)) {                                                      \
            ++g_tests_failed;                                               \
            std::fprintf(stderr, "  [FAIL] %s:%d: %s\n",                  \
                         __FILE__, __LINE__, #cond);                        \
        }                                                                   \
    } while(0)

#define SECTION(name) std::puts("  § " name)

// ─────────────────────────────────────────────────────────────────────────────
// §1 — Constantes de cacheline
// ─────────────────────────────────────────────────────────────────────────────
static void test_cacheline_constants() {
    SECTION("Test 1: constantes de cacheline");

    // kDestructivePad debe ser potencia de 2.
    constexpr bool is_pow2 = (hal::kDestructivePad & (hal::kDestructivePad - 1)) == 0;
    EXPECT(is_pow2);
    EXPECT(hal::kDestructivePad >= 64u);
    EXPECT(hal::kDestructivePad >= hal::kCachelineSize);
    EXPECT(hal::kMessageSlotSize == 64u);

    std::printf("    kCachelineSize=%zu, kDestructivePad=%zu, kMessageSlotSize=%zu\n",
                hal::kCachelineSize, hal::kDestructivePad, hal::kMessageSlotSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// §2 — rdtsc() monotónico
// ─────────────────────────────────────────────────────────────────────────────
static void test_rdtsc_monotonic() {
    SECTION("Test 2: rdtsc() es monótono");

    const hal::tsc_t t0 = hal::rdtsc();
    const hal::tsc_t t1 = hal::rdtsc();
    const hal::tsc_t t2 = hal::rdtsc();

    // Dos lecturas consecutivas no deben decrecer.
    EXPECT(t1 >= t0);
    EXPECT(t2 >= t1);

    std::printf("    t0=%llu, t1=%llu, t2=%llu (delta=%llu cycles)\n",
                static_cast<unsigned long long>(t0),
                static_cast<unsigned long long>(t1),
                static_cast<unsigned long long>(t2),
                static_cast<unsigned long long>(t2 - t0));
}

// ─────────────────────────────────────────────────────────────────────────────
// §3 — calibrate_tsc_freq() razonable
// ─────────────────────────────────────────────────────────────────────────────
static void test_calibrate_freq() {
    SECTION("Test 3: calibrate_tsc_freq() en [100 MHz, 10 GHz]");

    const hal::tsc_t freq = hal::calibrate_tsc_freq();

    EXPECT(freq > 100'000'000ULL);     // > 100 MHz
    EXPECT(freq < 10'000'000'000ULL);  // < 10 GHz

    std::printf("    TSC freq: ~%.0f MHz\n",
                static_cast<double>(freq) / 1e6);
}

// ─────────────────────────────────────────────────────────────────────────────
// §4 — tsc_to_ns() preciso para sleep de 10 ms
// ─────────────────────────────────────────────────────────────────────────────
static void test_tsc_to_ns() {
    SECTION("Test 4: tsc_to_ns() razonable para 10 ms sleep");

    // Calentar calibrate_tsc_freq (ya llamado en §3).
    const hal::tsc_t t0 = hal::rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const hal::tsc_t t1 = hal::rdtsc();

    const uint64_t elapsed_ns = hal::tsc_to_ns(t1 - t0);

    // Debe estar en [5 ms, 50 ms] — amplio para entornos lentos.
    EXPECT(elapsed_ns >=  5'000'000ULL);   // >= 5 ms
    EXPECT(elapsed_ns <= 50'000'000ULL);   // <= 50 ms

    std::printf("    10ms sleep → tsc_to_ns = %.2f ms\n",
                static_cast<double>(elapsed_ns) / 1e6);
}

// ─────────────────────────────────────────────────────────────────────────────
// §5 — seq_store_release / seq_load_acquire round-trip
// ─────────────────────────────────────────────────────────────────────────────
static void test_seq_roundtrip() {
    SECTION("Test 5: seq_store_release / seq_load_acquire round-trip");

    std::atomic<uint32_t> idx{0u};

    hal::seq_store_release(idx, 42u);
    const uint32_t v = hal::seq_load_acquire(idx);
    EXPECT(v == 42u);

    // ctrl aliases también.
    std::atomic<uint64_t> ctrl{0u};
    hal::ctrl_store_release(ctrl, UINT64_C(0xDEADBEEFCAFE));
    EXPECT(hal::ctrl_load_acquire(ctrl) == UINT64_C(0xDEADBEEFCAFE));
}

// ─────────────────────────────────────────────────────────────────────────────
// §6 — stat_fetch_add_relaxed acumula correctamente
// ─────────────────────────────────────────────────────────────────────────────
static void test_stat_counter() {
    SECTION("Test 6: stat_fetch_add_relaxed acumula correctamente");

    std::atomic<uint64_t> counter{0u};

    for (uint32_t i = 0u; i < 1000u; ++i) {
        hal::stat_fetch_add_relaxed(counter, uint64_t{1u});
    }

    const uint64_t val = hal::stat_load_relaxed(counter);
    EXPECT(val == 1000u);
    std::printf("    counter after 1000 adds = %llu\n",
                static_cast<unsigned long long>(val));
}

// ─────────────────────────────────────────────────────────────────────────────
// §7 — prefetch() no crashea
// ─────────────────────────────────────────────────────────────────────────────
static void test_prefetch_smoke() {
    SECTION("Test 7: prefetch() smoke — no crashea");

    // Prefetch de datos en el stack — siempre válido.
    int dummy[64] = {};
    hal::prefetch<hal::PrefetchHint::Read>(dummy);
    hal::prefetch<hal::PrefetchHint::Write>(dummy);
    hal::prefetch<hal::PrefetchHint::NonTemporal>(dummy);
    hal::prefetch_range<hal::PrefetchHint::Read>(dummy, 4u, sizeof(int));

    // Si llegamos aquí, no crasheó.
    EXPECT(true);
    std::puts("    prefetch() OK (instrucciones ejecutadas sin fault)");
}

// ─────────────────────────────────────────────────────────────────────────────
// §8 — simd_caps() detección smoke
// ─────────────────────────────────────────────────────────────────────────────
static void test_simd_caps_smoke() {
    SECTION("Test 8: simd_caps() smoke — detección sin crash");

    const hal::SimdCaps& caps = hal::simd_caps();

    // En x86 moderno se esperan estas capacidades.
    // En ARM o entorno desconocido simplemente verificamos que no crashea.
    std::printf("    simd_caps: avx2=%d avx512f=%d neon=%d sve=%d has_crc=%d\n",
                caps.avx2, caps.avx512f, caps.neon, caps.sve, caps.has_crc);

#if PHYRIAD_ARCH_X86_64
    // En x86 de producción (2020+): AVX2 es casi universal.
    // No forzamos el assert porque puede fallar en VMs sin AVX2.
    std::puts(caps.avx2 ? "    AVX2: disponible" : "    AVX2: no disponible (VM?)");
#elif PHYRIAD_ARCH_AARCH64
    EXPECT(caps.neon);  // NEON es obligatorio en aarch64.
#endif

    ++g_tests_run;  // Llegó aquí = pasó el smoke.
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::puts("[hal_types_test] phyriad_hal INTERFACE pillar");
    std::puts("----------------------------------------------------------------");
    std::printf("  sizeof(hal::SimdCaps) = %zu bytes\n", sizeof(hal::SimdCaps));
    std::printf("  kDestructivePad       = %zu bytes\n", hal::kDestructivePad);
    std::puts("----------------------------------------------------------------");

    test_cacheline_constants();
    test_rdtsc_monotonic();
    test_calibrate_freq();
    test_tsc_to_ns();
    test_seq_roundtrip();
    test_stat_counter();
    test_prefetch_smoke();
    test_simd_caps_smoke();

    std::puts("----------------------------------------------------------------");
    if (g_tests_failed == 0) {
        std::printf("[OK] %d/%d tests passed\n", g_tests_run, g_tests_run);
        return 0;
    } else {
        std::printf("[FAIL] %d/%d tests FAILED\n", g_tests_failed, g_tests_run);
        return 1;
    }
}
// Made with my soul - Swately <3
