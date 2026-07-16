// framework/etw/tests/etw_test.cpp
// Test suite for phyriad::etw::SessionManager.
//
// Tests:
//   1. Header compiles on all platforms (no-op stubs on Linux).
//   2. SessionManager default-constructs and is not running.
//   3. start_consumer() before start() returns Unavailable.
//   4. Windows: start() without admin returns PermissionDenied or succeeds.
//   5. Windows: stop() is safe to call when not started (no crash).
//   6. Windows: start() + stop() lifecycle — session_handle resets.
//
// Tests 4-6 are skipped on non-Windows (compilation is the only test there).
//

#include <phyriad/etw/SessionManager.hpp>
#include <phyriad/schema/Error.hpp>

#include <cstdio>
#include <type_traits>

// ── Micro-test framework ─────────────────────────────────────────────────────
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
// Test 1 — header compilation (cross-platform)
// ─────────────────────────────────────────────────────────────────────────────
static void test_header_compiles() {
    SECTION("Test 1: SessionManager header compiles (smoke test)");
    EXPECT(true);
    std::puts("    header included and compiled — OK");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — default-constructed SessionManager is not running
// ─────────────────────────────────────────────────────────────────────────────
static void test_default_not_running() {
    SECTION("Test 2: default-constructed SessionManager is not running");

    phyriad::etw::SessionManager s;
    EXPECT(!s.is_running());
    EXPECT(s.events_processed() == 0u);
    std::puts("    is_running()=false, events_processed()=0 — OK");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — start_consumer() before start() → Unavailable
// ─────────────────────────────────────────────────────────────────────────────
static void test_consumer_before_start() {
    SECTION("Test 3: start_consumer() before start() returns Unavailable");

    phyriad::etw::SessionManager s;
    auto r = s.start_consumer(nullptr, nullptr);
    EXPECT(!r.has_value());
    if (!r.has_value()) {
        EXPECT(r.error().code == phyriad::ErrorCode::Unavailable);
        std::printf("    got Unavailable (code=%u) — OK\n",
                    static_cast<unsigned>(r.error().code));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 — stop() on non-started session is safe
// ─────────────────────────────────────────────────────────────────────────────
static void test_stop_when_not_started() {
    SECTION("Test 4: stop() when not started — no crash");

    phyriad::etw::SessionManager s;
    s.stop(); // must not crash
    EXPECT(!s.is_running());
    std::puts("    stop() on idle session — OK");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — Windows: start() + stop() lifecycle
// ─────────────────────────────────────────────────────────────────────────────
static void test_start_stop_lifecycle() {
    SECTION("Test 5: Windows start() + stop() lifecycle");

#ifdef _WIN32
    phyriad::etw::SessionManager s;
    phyriad::etw::ProviderSpec providers[] = {
        {phyriad::etw::providers::kKernelProcess,
         4u /*TRACE_LEVEL_INFORMATION*/,
         0x10u /* PROCESS keyword */,
         0u}
    };

    auto r = s.start("phyriad_etw_test_session", providers);
    if (!r.has_value()) {
        if (r.error().code == phyriad::ErrorCode::PermissionDenied) {
            std::puts("    start() → PermissionDenied (not admin) — SKIPPED");
            EXPECT(true); // expected on non-admin CI
            return;
        }
        std::fprintf(stderr, "    start() failed unexpectedly: code=%u\n",
                     static_cast<unsigned>(r.error().code));
        EXPECT(false);
        return;
    }

    EXPECT(s.is_running());
    std::puts("    start() OK — session running");

    s.stop();
    EXPECT(!s.is_running());
    std::puts("    stop() OK — session stopped");
#else
    // On non-Windows, start() returns Unavailable — verify that.
    phyriad::etw::SessionManager s;
    auto r = s.start("test_session");
    EXPECT(!r.has_value());
    if (!r.has_value()) {
        EXPECT(r.error().code == phyriad::ErrorCode::Unavailable);
        std::printf("    non-Windows: got Unavailable (code=%u) — OK\n",
                    static_cast<unsigned>(r.error().code));
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 — non-copyable / non-movable
// ─────────────────────────────────────────────────────────────────────────────
static void test_not_copyable() {
    SECTION("Test 6: SessionManager is not copyable (compile-time)");

    static_assert(!std::is_copy_constructible_v<phyriad::etw::SessionManager>,
        "SessionManager must not be copy-constructible");
    static_assert(!std::is_copy_assignable_v<phyriad::etw::SessionManager>,
        "SessionManager must not be copy-assignable");
    EXPECT(true);
    std::puts("    not copyable — OK");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::puts("[etw_test] phyriad_etw pillar — Phase 6.A");
    std::puts("----------------------------------------------------------------");
#ifdef _WIN32
    std::puts("  Platform: Windows (full implementation)");
#else
    std::puts("  Platform: non-Windows (no-op stubs)");
#endif
    std::puts("----------------------------------------------------------------");

    test_header_compiles();
    test_default_not_running();
    test_consumer_before_start();
    test_stop_when_not_started();
    test_start_stop_lifecycle();
    test_not_copyable();

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
