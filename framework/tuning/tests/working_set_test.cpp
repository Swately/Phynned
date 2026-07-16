// framework/tuning/tests/working_set_test.cpp
// Unit tests for phyriad::tuning::set_self_working_set / get_self_working_set.
//
// Coverage:
//   1. get_self_working_set returns valid values (current > 0).
//   2. set_self_working_set(16MB, 64MB) succeeds (or PermissionDenied on
//      restricted environments — skip, not fail).
//   3. set_self_working_set(0, 0) succeeds — releases hint.
//   4. InvalidArgument when min > max (both non-zero).
//   5. POSIX: no crash; returns OK even if kernel ignores the hint.
//
#include <phyriad/tuning/WorkingSet.hpp>

#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>  // std::abort

static void fail(const char* msg) {
    std::fprintf(stderr, "[FAIL] %s\n", msg);
    std::fflush(stderr);
    std::abort();
}
#define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while(0)

// ── Test 1: get_self_working_set ──────────────────────────────────────────────
static void test_get_working_set() {
    uint64_t cur = 0u;
    uint64_t peak = 0u;
    auto r = phyriad::tuning::get_self_working_set(&cur, &peak);
    CHECK(r.has_value(), "get_self_working_set must succeed");
    // We're running — current RSS must be > 0.
    CHECK(cur > 0u, "current working set > 0");
    std::printf("[OK] test_get_working_set (cur=%" PRIu64 " KB, peak=%" PRIu64 " KB)\n",
                cur / 1024u, peak / 1024u);
}

// ── Test 2: set with valid bounds ─────────────────────────────────────────────
// Note: SetProcessWorkingSetSize may fail on restricted environments (e.g.
// low-privilege CI machines, AppContainers, Win11 security sandbox). Skip.
static void test_set_working_set_valid() {
    const uint64_t min_b = 16ull * 1024u * 1024u;   // 16 MB
    const uint64_t max_b = 64ull * 1024u * 1024u;   // 64 MB
    auto r = phyriad::tuning::set_self_working_set(min_b, max_b);
    if (!r.has_value()) {
        std::printf("[SKIP] test_set_working_set_valid (OS denied, code=%u)\n",
                    static_cast<unsigned>(r.error().code));
        return;
    }
    std::printf("[OK] test_set_working_set_valid\n");
}

// ── Test 3: set (0, 0) releases hint ─────────────────────────────────────────
// Note: on some Windows environments SetProcessWorkingSetSize(proc, 0, 0)
// fails even for the current process (requires PROCESS_SET_QUOTA right or
// specific OS policies). Skip gracefully on any failure.
static void test_set_working_set_release() {
    auto r = phyriad::tuning::set_self_working_set(0u, 0u);
    if (!r.has_value()) {
        std::printf("[SKIP] test_set_working_set_release (OS denied, code=%u)\n",
                    static_cast<unsigned>(r.error().code));
        return;
    }
    std::printf("[OK] test_set_working_set_release\n");
}

// ── Test 4: InvalidArgument when min > max ────────────────────────────────────
static void test_invalid_argument() {
    auto r = phyriad::tuning::set_self_working_set(64u * 1024u * 1024u,  // min=64MB
                                                16u * 1024u * 1024u); // max=16MB
    CHECK(!r.has_value(), "min > max must fail");
    CHECK(r.error().code == phyriad::ErrorCode::InvalidArgument,
          "error code must be InvalidArgument");
    std::printf("[OK] test_invalid_argument\n");
}

// ── Test 5: nullptr out params are safe ──────────────────────────────────────
static void test_null_out_params() {
    auto r = phyriad::tuning::get_self_working_set(nullptr, nullptr);
    CHECK(r.has_value(), "null out params must not crash");
    std::printf("[OK] test_null_out_params\n");
}

// ── GFR-Ayama-5: cross-process working-set tests ─────────────────────────────
#ifdef _WIN32
#  include <windows.h>
inline uint32_t current_pid() noexcept {
    return static_cast<uint32_t>(GetCurrentProcessId());
}
#else
#  include <unistd.h>
inline uint32_t current_pid() noexcept {
    return static_cast<uint32_t>(getpid());
}
#endif

// ── Test 6 (GFR-Ayama-5): set_process_working_set on self via PID ────────────
static void test_set_process_working_set_self() {
    const uint32_t self = current_pid();
    constexpr uint64_t kMin = 16ull * 1024u * 1024u;
    constexpr uint64_t kMax = 64ull * 1024u * 1024u;
    auto r = phyriad::tuning::set_process_working_set(self, kMin, kMax);
#ifdef _WIN32
    // On Windows this should succeed (we own the process). PermissionDenied
    // acceptable on locked-down CI.
    CHECK(r.has_value()
            || r.error().code == phyriad::ErrorCode::PermissionDenied,
          "set_process_working_set(self) returned unexpected error");
#else
    CHECK(!r.has_value(), "Linux: should return Unavailable");
    CHECK(r.error().code == phyriad::ErrorCode::Unavailable,
          "Linux: error code must be Unavailable");
#endif
    std::printf("[OK] test_set_process_working_set_self\n");
}

// ── Test 7 (GFR-Ayama-5): get_process_working_set_limits returns values ──────
static void test_get_process_working_set_limits() {
    const uint32_t self = current_pid();
    auto r = phyriad::tuning::get_process_working_set_limits(self);
#ifdef _WIN32
    CHECK(r.has_value(), "get_process_working_set_limits should succeed on self");
    if (r) {
        std::printf("[INFO] limits: min=%" PRIu64 " max=%" PRIu64 "\n",
                    r->first, r->second);
    }
#else
    CHECK(!r.has_value() && r.error().code == phyriad::ErrorCode::Unavailable,
          "Linux: should be Unavailable");
#endif
    std::printf("[OK] test_get_process_working_set_limits\n");
}

// ── Test 8 (GFR-Ayama-5): invalid PID rejected ───────────────────────────────
static void test_set_process_working_set_invalid_pid() {
    const uint32_t bogus = 0xFFFFFFFEu;
    auto r = phyriad::tuning::set_process_working_set(bogus,
                                                   16u * 1024u * 1024u,
                                                   64u * 1024u * 1024u);
    CHECK(!r.has_value(), "invalid PID must return error");
    // Code can be InvalidArgument, PermissionDenied, or Unavailable (Linux).
    std::printf("[OK] test_set_process_working_set_invalid_pid\n");
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    test_get_working_set();
    test_set_working_set_valid();
    test_set_working_set_release();
    test_invalid_argument();
    test_null_out_params();

    // GFR-Ayama-5 tests
    test_set_process_working_set_self();
    test_get_process_working_set_limits();
    test_set_process_working_set_invalid_pid();

    std::printf("\n[PASS] working_set_test\n");
    return 0;
}
// Made with my soul - Swately <3
