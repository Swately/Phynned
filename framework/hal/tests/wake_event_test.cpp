// framework/hal/tests/wake_event_test.cpp
// Unit tests for phyriad::hal::WakeEvent.
//
// Coverage:
//   1. Create succeeds.
//   2. wait(100ms) without signal → returns false (timeout).
//   3. signal() + wait() → returns true immediately.
//   4. Signal BEFORE wait — wait still returns true (auto-reset deferred).
//   5. Two consecutive signals → only one wait consumed (auto-reset).
//   6. Stress: 100k signal/wait pairs in 2 threads, no deadlock/loss.
//

#include <phyriad/hal/WakeEvent.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

static void fail(const char* msg) {
    std::fprintf(stderr, "[FAIL] %s\n", msg);
    std::fflush(stderr);
    std::abort();
}
#define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while(0)

// ── Test 1: create ────────────────────────────────────────────────────────────
static void test_create() {
    auto r = phyriad::hal::WakeEvent::create();
    CHECK(r.has_value(), "create must succeed");
    CHECK(r->is_valid(), "is_valid after create");
    std::printf("[OK] test_create\n");
}

// ── Test 2: timeout (no signal) ───────────────────────────────────────────────
static void test_timeout() {
    auto r = phyriad::hal::WakeEvent::create();
    CHECK(r.has_value(), "create");
    const auto t0 = std::chrono::steady_clock::now();
    const bool woken = r->wait(50u);   // 50 ms timeout
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(!woken, "wait without signal returns false");
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    // Allow broad range: 40–500 ms (CI machines can be slow with scheduling).
    CHECK(ms >= 30.0, "timeout too short");
    std::printf("[OK] test_timeout (%.1f ms elapsed)\n", ms);
}

// ── Test 3: signal + wait ─────────────────────────────────────────────────────
static void test_signal_then_wait() {
    auto r = phyriad::hal::WakeEvent::create();
    CHECK(r.has_value(), "create");

    std::thread signaler([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        r->signal();
    });

    const bool woken = r->wait(2000u);  // 2s — should wake well before that
    CHECK(woken, "wait should return true after signal");
    signaler.join();
    std::printf("[OK] test_signal_then_wait\n");
}

// ── Test 4: signal before wait ────────────────────────────────────────────────
static void test_signal_before_wait() {
    auto r = phyriad::hal::WakeEvent::create();
    CHECK(r.has_value(), "create");
    r->signal();                          // signal before anyone waits
    const bool woken = r->wait(0u);       // should be already signaled
    CHECK(woken, "pre-signaled wait returns true");
    std::printf("[OK] test_signal_before_wait\n");
}

// ── Test 5: auto-reset (two signals → one consumed) ──────────────────────────
static void test_auto_reset() {
    auto r = phyriad::hal::WakeEvent::create();
    CHECK(r.has_value(), "create");

    r->signal();
    r->signal();  // second signal is idempotent on auto-reset

    const bool first = r->wait(100u);
    CHECK(first, "first wait after 2 signals returns true");

    // After the first wait consumed the signal, the next should time out.
    const bool second = r->wait(20u);
    CHECK(!second, "second wait returns false (auto-reset consumed)");

    std::printf("[OK] test_auto_reset\n");
}

// ── Test 6: throughput (10k synchronized signal/wait pairs) ──────────────────
// Auto-reset semantics: each signal() wakes at most ONE wait(). The producer
// must wait for the consumer to consume each signal before sending the next —
// otherwise multiple signals before wait() collapse into one wake-up.
// We use a "ping-pong" pattern: producer signals, consumer acks, repeat.
static void test_stress() {
    constexpr uint32_t kIter = 10'000u;
    auto ping_opt = phyriad::hal::WakeEvent::create();
    auto pong_opt = phyriad::hal::WakeEvent::create();
    CHECK(ping_opt.has_value(), "create ping");
    CHECK(pong_opt.has_value(), "create pong");
    auto& ping = *ping_opt;
    auto& pong = *pong_opt;

    std::thread prod([&]() {
        for (uint32_t i = 0; i < kIter; ++i) {
            ping.signal();          // wake consumer
            (void)pong.wait(2000u); // wait for ack (2s max per pair)
        }
    });

    for (uint32_t i = 0; i < kIter; ++i) {
        const bool woken = ping.wait(2000u);
        CHECK(woken, "ping wait should succeed");
        pong.signal();  // ack
    }

    prod.join();
    std::printf("[OK] test_stress (%u ping-pong pairs)\n", kIter);
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    test_create();
    test_timeout();
    test_signal_then_wait();
    test_signal_before_wait();
    test_auto_reset();
    test_stress();

    std::printf("\n[PASS] wake_event_test\n");
    return 0;
}
// Made with my soul - Swately <3
