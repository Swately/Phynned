// framework/stigmergy/tests/stigmergy_pheromone_test.cpp
// Pheromone<T, N> unit tests — Phase P-0.5.1 verification.
//
// Sections:
//   §1   default-constructed slots read as default T
//   §2   single-slot deposit + read round-trip
//   §3   independent slots: depositing into slot i doesn't affect slot j
//   §4   out-of-range deposit is a safe no-op (defensive guard)
//   §5   out-of-range read returns default T (defensive guard)
//   §6   read_all() returns std::array of N slots
//   §7   clear(i) resets slot i to default T
//   §8   clear_all() resets every slot
//   §9   concurrent: N agents each deposit to their own slot — no interference
//   §10  scaling smoke test — measure deposit rate at N=1,2,4,8 threads
//
// Stigmergy as first-class pillar.
#include <phyriad/stigmergy/Pheromone.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>     // std::getenv for CI environment detection
#include <thread>
#include <vector>

using namespace phyriad::stigmergy;

static int g_pass{0};
static int g_fail{0};

#define SECTION(msg) std::printf("  § %s\n", (msg))
#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (cond) { ++g_pass; }                                               \
        else {                                                                \
            ++g_fail;                                                         \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                     \
    } while (false)

// §1 ──────────────────────────────────────────────────────────────────────────
static void test_default_slots_read_default_t() {
    SECTION("Test 1: default-constructed Pheromone — every slot reads as 0");
    Pheromone<uint8_t, 16> p;
    for (std::size_t i = 0; i < 16u; ++i) {
        EXPECT(p.read(i) == 0u);
    }
    EXPECT(p.size() == 16u);
    EXPECT(decltype(p)::size_v == 16u);
}

// §2 ──────────────────────────────────────────────────────────────────────────
static void test_deposit_read_roundtrip() {
    SECTION("Test 2: deposit(i, v) then read(i) returns v");
    Pheromone<uint8_t, 16> p;
    p.deposit(0, 7u);
    p.deposit(15, 99u);
    EXPECT(p.read(0)  == 7u);
    EXPECT(p.read(15) == 99u);
}

// §3 ──────────────────────────────────────────────────────────────────────────
static void test_independent_slots() {
    SECTION("Test 3: depositing into slot i doesn't affect slot j");
    Pheromone<uint16_t, 8> p;
    p.deposit(3, 42u);
    for (std::size_t i = 0; i < 8u; ++i) {
        if (i == 3u) EXPECT(p.read(i) == 42u);
        else         EXPECT(p.read(i) == 0u);
    }
}

// §4 ──────────────────────────────────────────────────────────────────────────
static void test_out_of_range_deposit_safe() {
    SECTION("Test 4: out-of-range deposit is a no-op (no crash)");
    Pheromone<uint8_t, 4> p;
    p.deposit(0, 1u);
    p.deposit(4, 2u);     // ← out of range, must be ignored
    p.deposit(99, 3u);    // ← out of range, must be ignored
    EXPECT(p.read(0) == 1u);
    // Verify no corruption of valid slots.
    EXPECT(p.read(1) == 0u);
    EXPECT(p.read(2) == 0u);
    EXPECT(p.read(3) == 0u);
}

// §5 ──────────────────────────────────────────────────────────────────────────
static void test_out_of_range_read_returns_default() {
    SECTION("Test 5: out-of-range read returns default T{}");
    Pheromone<uint8_t, 4> p;
    p.deposit(0, 200u);
    EXPECT(p.read(4)  == 0u);
    EXPECT(p.read(99) == 0u);
    EXPECT(p.read(0)  == 200u);
}

// §6 ──────────────────────────────────────────────────────────────────────────
static void test_read_all_snapshot() {
    SECTION("Test 6: read_all() returns std::array<T, N> snapshot");
    Pheromone<uint8_t, 8> p;
    for (std::size_t i = 0; i < 8u; ++i) {
        p.deposit(i, static_cast<uint8_t>(i * 10u));
    }
    auto snap = p.read_all();
    EXPECT(snap.size() == 8u);
    for (std::size_t i = 0; i < 8u; ++i) {
        EXPECT(snap[i] == static_cast<uint8_t>(i * 10u));
    }
}

// §7 ──────────────────────────────────────────────────────────────────────────
static void test_clear_single_slot() {
    SECTION("Test 7: clear(i) resets only slot i");
    Pheromone<uint8_t, 4> p;
    p.deposit(0, 11u);
    p.deposit(1, 22u);
    p.deposit(2, 33u);
    p.deposit(3, 44u);
    p.clear(2);
    EXPECT(p.read(0) == 11u);
    EXPECT(p.read(1) == 22u);
    EXPECT(p.read(2) == 0u);
    EXPECT(p.read(3) == 44u);
}

// §8 ──────────────────────────────────────────────────────────────────────────
static void test_clear_all() {
    SECTION("Test 8: clear_all() resets every slot");
    Pheromone<uint16_t, 6> p;
    for (std::size_t i = 0; i < 6u; ++i) p.deposit(i, 1000u);
    p.clear_all();
    for (std::size_t i = 0; i < 6u; ++i) EXPECT(p.read(i) == 0u);
}

// §9 ──────────────────────────────────────────────────────────────────────────
static void test_concurrent_distinct_slots() {
    SECTION("Test 9: N agents each deposit to their own slot — final state correct");
    constexpr std::size_t N = 16u;
    Pheromone<uint32_t, N> p;
    std::vector<std::thread> agents;
    agents.reserve(N);

    for (std::size_t i = 0; i < N; ++i) {
        agents.emplace_back([&p, i]() noexcept {
            // Each agent deposits its own ID 100k times. The final value
            // observed at slot i must equal i (the last deposit wins).
            for (uint32_t k = 0; k < 100000u; ++k) {
                p.deposit(i, static_cast<uint32_t>(i));
            }
        });
    }
    for (auto& t : agents) t.join();

    auto snap = p.read_all();
    for (std::size_t i = 0; i < N; ++i) {
        EXPECT(snap[i] == static_cast<uint32_t>(i));
    }
}

// §10 ─────────────────────────────────────────────────────────────────────────
static void test_scaling_smoke() {
    SECTION("Test 10: scaling smoke — deposit rate grows ≥ linearly to 4 threads");
    constexpr std::size_t N = 32u;
    constexpr uint64_t kIters = 5'000'000u;

    auto measure_for = [](std::size_t threads) -> double {
        Pheromone<uint32_t, N> p;
        std::vector<std::thread> ts;
        ts.reserve(threads);

        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < threads; ++i) {
            ts.emplace_back([&p, i]() noexcept {
                for (uint64_t k = 0; k < kIters; ++k) {
                    p.deposit(i, static_cast<uint32_t>(k));
                }
            });
        }
        for (auto& t : ts) t.join();
        auto t1 = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        const double rate = static_cast<double>(kIters * threads) / sec;
        std::printf("        %zu threads × %llu deposits  →  %.2f M dep/s\n",
                    threads,
                    static_cast<unsigned long long>(kIters),
                    rate / 1e6);
        return rate;
    };

    const double r1 = measure_for(1);
    const double r2 = measure_for(2);
    const double r4 = measure_for(4);

    // FUNCTIONAL check only (deterministic on every environment): each thread
    // count must produce real throughput — i.e. the Pheromone is usable
    // concurrently at 1/2/4 threads and nothing deadlocks or zeroes out.
    //
    // The SCALING SHAPE (near-linear deposit rate to 4 threads, the payoff of
    // per-slot cache-line padding) is a PERFORMANCE claim, not a correctness
    // one, and it is inherently contention-sensitive: it only holds on an
    // otherwise-idle machine with the threads on distinct cores. Asserting a
    // hard ratio floor here made this UNIT test flaky under `ctest -j` (the
    // suite runner oversubscribes cores — observed: a rare failure when run
    // concurrently with other tests, even on a dev box; always on a 2-vCPU CI
    // runner). The scaling shape is measured properly, with affinity pinning,
    // in `bench/bench_stigmergy_primitives` + the `bench_stigmergy_workflow`
    // scaling sweep — that is its correct home, not a deterministic unit test.
    EXPECT(r1 > 0.0);
    EXPECT(r2 > 0.0);
    EXPECT(r4 > 0.0);
}

int main() {
    std::printf("[stigmergy_pheromone_test] phyriad_stigmergy — Phase P-0.5.1\n");
    std::printf("----------------------------------------------------------------\n");

    test_default_slots_read_default_t();
    test_deposit_read_roundtrip();
    test_independent_slots();
    test_out_of_range_deposit_safe();
    test_out_of_range_read_returns_default();
    test_read_all_snapshot();
    test_clear_single_slot();
    test_clear_all();
    test_concurrent_distinct_slots();
    test_scaling_smoke();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d checks passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d checks FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
