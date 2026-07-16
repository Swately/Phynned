// framework/runtime/tests/heartbeat_test.cpp
// HeartbeatArray tests — Phase 4 restored-feature verification.
//
// Sections:
//   §1  Default ctor produces empty array (size==0)
//   §2  Sized ctor allocates N cache-line-aligned slots
//   §3  tick() increments only the target slot, leaves neighbours alone
//   §4  read() returns the latest counter
//   §5  cursor() exposes the atomic for spin-wait callers
//   §6  Out-of-range tick() / read() are no-ops (no UB)
//   §7  Concurrent ticks from many threads sum to expected total
//

#include <phyriad/runtime/Heartbeat.hpp>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <vector>
#include <phyriad/hal/MemoryOrder.hpp>

using namespace phyriad::runtime;

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

// ── §1 Default ctor ──────────────────────────────────────────────────────────
static void test_default_ctor() {
    SECTION("Test 1: HeartbeatArray() — empty");
    HeartbeatArray hb;
    EXPECT(hb.size() == 0u);
    // Out-of-range read returns 0 without UB.
    EXPECT(hb.read(0u) == 0u);
}

// ── §2 Sized ctor ────────────────────────────────────────────────────────────
static void test_sized_ctor() {
    SECTION("Test 2: HeartbeatArray(N) — N slots, all counters at 0");
    HeartbeatArray hb{8u};
    EXPECT(hb.size() == 8u);
    for (std::size_t i = 0; i < hb.size(); ++i)
        EXPECT(hb.read(i) == 0u);
}

// ── §3 tick increments only target ───────────────────────────────────────────
static void test_tick_increments() {
    SECTION("Test 3: tick(i) increments slot i; neighbours unchanged");
    HeartbeatArray hb{4u};
    hb.tick(2u);
    EXPECT(hb.read(0u) == 0u);
    EXPECT(hb.read(1u) == 0u);
    EXPECT(hb.read(2u) == 1u);
    EXPECT(hb.read(3u) == 0u);

    hb.tick(2u);
    hb.tick(2u);
    EXPECT(hb.read(2u) == 3u);
}

// ── §4 read returns latest ───────────────────────────────────────────────────
static void test_read_latest() {
    SECTION("Test 4: read() returns the latest counter after several ticks");
    HeartbeatArray hb{2u};
    for (int i = 0; i < 100; ++i) hb.tick(0u);
    EXPECT(hb.read(0u) == 100u);
    EXPECT(hb.read(1u) == 0u);
}

// ── §5 cursor exposes atomic ─────────────────────────────────────────────────
static void test_cursor() {
    SECTION("Test 5: cursor(i) exposes the atomic for spin-wait callers");
    HeartbeatArray hb{1u};
    hb.tick(0u);
    auto& c = hb.cursor(0u);
    EXPECT(phyriad::hal::stat_load_relaxed(c) == 1u);
}

// ── §6 Out-of-range is a no-op ───────────────────────────────────────────────
static void test_out_of_range_noop() {
    SECTION("Test 6: out-of-range tick/read are no-ops");
    HeartbeatArray hb{2u};
    hb.tick(99u);     // must not crash
    EXPECT(hb.read(99u) == 0u);
    EXPECT(hb.read(0u) == 0u);
    EXPECT(hb.read(1u) == 0u);
}

// ── §7 Concurrent ticks ──────────────────────────────────────────────────────
static void test_concurrent_ticks() {
    SECTION("Test 7: concurrent ticks from many threads sum correctly");
    HeartbeatArray hb{8u};

    constexpr int kThreads     = 4;
    constexpr int kTicksPerThr = 10'000;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&hb]() noexcept {
            for (int i = 0; i < kTicksPerThr; ++i) hb.tick(3u);
        });
    }
    for (auto& w : workers) w.join();

    EXPECT(hb.read(3u) == static_cast<uint64_t>(kThreads * kTicksPerThr));
    // Other slots remain untouched.
    EXPECT(hb.read(0u) == 0u);
    EXPECT(hb.read(7u) == 0u);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::printf("[heartbeat_test] phyriad_runtime — Phase 4\n");
    std::printf("----------------------------------------------------------------\n");

    test_default_ctor();
    test_sized_ctor();
    test_tick_increments();
    test_read_latest();
    test_cursor();
    test_out_of_range_noop();
    test_concurrent_ticks();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d checks passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d checks FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
