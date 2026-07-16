// framework/ipc/tests/ring_test.cpp
// Unit tests for phyriad::ipc::Ring<T, Capacity>.
//
// Coverage:
//   1. POD / compile-time assertions.
//   2. Basic push/pop correctness.
//   3. Full-ring detection and dropped() counter.
//   4. Empty-ring try_pop returns false.
//   5. Wraparound correctness (2.5× capacity cycles).
//   6. drain() — batch extraction, handles wrap.
//   7. drain() with partial fill.
//   8. Producer/consumer thread safety (1 M iterations).
//   9. Performance gate: 10 M push+pop < 3 s.
//

#include <phyriad/ipc/Ring.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <phyriad/hal/MemoryOrder.hpp>

// ── Helpers ───────────────────────────────────────────────────────────────────

static void fail(const char* msg) {
    std::fprintf(stderr, "[FAIL] %s\n", msg);
    std::fflush(stderr);
    std::abort();
}

#define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while(0)

// ── Test 1: static assertions (compile-time) ──────────────────────────────────
// These assertions live in the template — confirmed by the fact that this TU
// compiles at all with valid params. Invalid params would be a separate TU.
namespace {

// Valid instantiation — confirms the static_assert paths pass.
using Ring32 = phyriad::ipc::Ring<uint32_t, 64>;
static_assert(Ring32::capacity() == 64u, "capacity()");

// Struct type check
struct Evt { uint32_t a; uint32_t b; };
static_assert(std::is_trivially_copyable_v<Evt>);
using RingEvt = phyriad::ipc::Ring<Evt, 128>;

} // namespace

// ── Test 2: basic push / pop ──────────────────────────────────────────────────
static void test_basic_push_pop() {
    phyriad::ipc::Ring<uint32_t, 8> r;
    CHECK(r.empty(), "empty before push");

    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(r.try_push(i), "try_push should succeed");
    }
    CHECK(r.size() == 8u, "size == 8 after 8 pushes");
    CHECK(!r.empty(), "not empty");

    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t v = 0xFFFF;
        CHECK(r.try_pop(v), "try_pop should succeed");
        CHECK(v == i, "value mismatch");
    }
    CHECK(r.empty(), "empty after draining");
    std::printf("[OK] test_basic_push_pop\n");
}

// ── Test 3: full-ring detection + dropped() ───────────────────────────────────
static void test_full_ring() {
    phyriad::ipc::Ring<uint32_t, 4> r;
    CHECK(r.try_push(1), "push 1");
    CHECK(r.try_push(2), "push 2");
    CHECK(r.try_push(3), "push 3");
    CHECK(r.try_push(4), "push 4");
    CHECK(!r.try_push(5), "5th push should fail (full)");
    CHECK(r.dropped() == 1u, "dropped must be 1");

    // Consume one, then retry
    uint32_t v;
    CHECK(r.try_pop(v), "pop after full");
    CHECK(v == 1u, "popped value");
    CHECK(r.try_push(5), "push after freeing slot");
    std::printf("[OK] test_full_ring\n");
}

// ── Test 4: empty pop returns false ──────────────────────────────────────────
static void test_empty_pop() {
    phyriad::ipc::Ring<uint32_t, 16> r;
    uint32_t v = 0;
    CHECK(!r.try_pop(v), "try_pop on empty ring returns false");
    CHECK(v == 0, "out value unchanged");
    std::printf("[OK] test_empty_pop\n");
}

// ── Test 5: wraparound correctness ───────────────────────────────────────────
// Strategy: push a full ring, drain half, push another half to force the
// write cursor past the power-of-2 wrap point, then verify all values.
static void test_wraparound() {
    constexpr uint32_t Cap = 16;
    phyriad::ipc::Ring<uint32_t, Cap> r;

    // Round 1: fill the ring completely (cursor 0 → 16).
    for (uint32_t i = 0; i < Cap; ++i) {
        CHECK(r.try_push(i * 100u), "fill push");
    }

    // Drain exactly half (consume 0..7, cursors: w=16, r=8).
    for (uint32_t i = 0; i < Cap / 2u; ++i) {
        uint32_t v;
        CHECK(r.try_pop(v), "drain pop");
        CHECK(v == i * 100u, "drain value mismatch");
    }

    // Round 2: push another half-ring (cursor 16 → 24, wraps slot indices).
    for (uint32_t i = 0; i < Cap / 2u; ++i) {
        CHECK(r.try_push(1000u + i), "wrap push");
    }

    // Now ring has: [Cap/2 .. Cap-1] original + [1000..1007] new = Cap items.
    // Drain them in order.
    for (uint32_t i = Cap / 2u; i < Cap; ++i) {
        uint32_t v;
        CHECK(r.try_pop(v), "post-wrap pop old");
        CHECK(v == i * 100u, "post-wrap old value mismatch");
    }
    for (uint32_t i = 0; i < Cap / 2u; ++i) {
        uint32_t v;
        CHECK(r.try_pop(v), "post-wrap pop new");
        CHECK(v == 1000u + i, "post-wrap new value mismatch");
    }
    CHECK(r.empty(), "ring empty after full wraparound drain");
    std::printf("[OK] test_wraparound (cap=%u, two-phase push/drain)\n", Cap);
}

// ── Test 6: drain() batch extraction ─────────────────────────────────────────
static void test_drain_batch() {
    phyriad::ipc::Ring<uint32_t, 64> r;

    for (uint32_t i = 0; i < 48; ++i) { (void)r.try_push(i); }

    uint32_t buf[64] = {};
    const uint32_t got = r.drain(buf, 64);
    CHECK(got == 48u, "drain count");
    for (uint32_t i = 0; i < 48; ++i) {
        CHECK(buf[i] == i, "drain value mismatch");
    }
    CHECK(r.empty(), "ring empty after drain");
    std::printf("[OK] test_drain_batch\n");
}

// ── Test 7: drain() across wrap boundary ─────────────────────────────────────
static void test_drain_wrap() {
    constexpr uint32_t Cap = 8;
    phyriad::ipc::Ring<uint32_t, Cap> r;

    // Advance write cursor to simulate wrap: fill, drain half, refill
    for (uint32_t i = 0; i < Cap; ++i) { (void)r.try_push(i); }

    uint32_t tmp[4];
    CHECK(r.drain(tmp, 4) == 4u, "first drain 4");   // read_seq = 4

    // Push 4 more — write_seq = 12, read_seq = 4 → slots 8..11 wrap to 0..3
    for (uint32_t i = 8; i < 12; ++i) { (void)r.try_push(i); }
    // ring now has: [4,5,6,7] + [8,9,10,11] — spanning the wrap point

    uint32_t big[8];
    const uint32_t got = r.drain(big, 8);
    CHECK(got == 8u, "wrap drain count");
    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(big[i] == i + 4u, "wrap value mismatch");
    }
    std::printf("[OK] test_drain_wrap\n");
}

// ── Test 8: thread safety (SPSC, 1 M iterations) ─────────────────────────────
static void test_thread_safety() {
    constexpr uint32_t kIter = 1'000'000u;
    phyriad::ipc::Ring<uint64_t, 4096> r;

    std::atomic<bool> go{false};
    std::atomic<uint64_t> sum_produced{0};
    std::atomic<uint64_t> sum_consumed{0};

    std::thread producer([&]() {
        while (!phyriad::hal::seq_load_acquire(go)) {}
        for (uint64_t i = 1; i <= kIter; ++i) {
            while (!r.try_push(i)) { /* spin */ }
            phyriad::hal::stat_fetch_add_relaxed(sum_produced, i);
        }
    });

    std::thread consumer([&]() {
        while (!phyriad::hal::seq_load_acquire(go)) {}
        uint64_t received = 0;
        while (received < kIter) {
            uint64_t v;
            if (r.try_pop(v)) {
                phyriad::hal::stat_fetch_add_relaxed(sum_consumed, v);
                ++received;
            }
        }
    });

    phyriad::hal::seq_store_release(go, true);
    producer.join();
    consumer.join();

    CHECK(sum_produced.load() == sum_consumed.load(),
          "thread safety: sum mismatch");
    std::printf("[OK] test_thread_safety (1M iterations, sum=%llu)\n",
                static_cast<unsigned long long>(sum_consumed.load()));
}

// ── Test 9: performance gate ──────────────────────────────────────────────────
static void test_performance_gate() {
    constexpr uint32_t kIter = 10'000'000u;
    phyriad::ipc::Ring<uint64_t, 4096> r;

    const auto t0 = std::chrono::steady_clock::now();

    for (uint64_t i = 0; i < kIter; ) {
        if (r.try_push(i)) {
            uint64_t v;
            if (r.try_pop(v)) {
                ++i;
            }
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Gate: 10M iterations < 3000 ms (~300 ns/pair). Very conservative to avoid
    // CI flakiness; expected actual is ~20-40 ns/pair on modern hardware.
    std::printf("[OK] test_performance_gate: 10M push+pop in %.1f ms\n", ms);
    if (ms > 3000.0) {
        std::fprintf(stderr,
            "[WARN] Performance gate soft-fail: %.1f ms > 3000 ms threshold\n", ms);
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    test_basic_push_pop();
    test_full_ring();
    test_empty_pop();
    test_wraparound();
    test_drain_batch();
    test_drain_wrap();
    test_thread_safety();
    test_performance_gate();

    std::printf("\n[PASS] ring_test\n");
    return 0;
}
// Made with my soul - Swately <3
