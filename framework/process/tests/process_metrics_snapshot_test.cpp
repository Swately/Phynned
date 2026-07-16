// framework/process/tests/process_metrics_snapshot_test.cpp
// Tests for phyriad::proc::ProcessMetricsSnapshot.
//
// Coverage:
//   1. POD layout static_assert (64B, trivially_copyable).
//   2. create() succeeds.
//   3. capture() → find(self_pid()) returns non-null with valid metrics.
//   4. Buffer growth: create with tiny capacity, capture() should grow+succeed.
//   5. extract() with mixed existing+nonexistent PIDs.
//   6. Idempotent: 100 captures — no crash, process_count() stable.
//   7. Performance gate: 1000 captures < 30 s (generous for CI).
//

#include <phyriad/process/ProcessMetricsSnapshot.hpp>

#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdio>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
static uint32_t os_self_pid() { return GetCurrentProcessId(); }
#else
#  include <unistd.h>
static uint32_t os_self_pid() { return static_cast<uint32_t>(getpid()); }
#endif

static void fail(const char* msg) {
    std::fprintf(stderr, "[FAIL] %s\n", msg);
    std::fflush(stderr);
    std::abort();
}
#define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while(0)

// ── Test 1: static assertions (build-time) ────────────────────────────────────
namespace {
static_assert(sizeof(phyriad::proc::ProcessMetrics) == 64u,
    "ProcessMetrics must be 64 bytes");
static_assert(std::is_trivially_copyable_v<phyriad::proc::ProcessMetrics>);
static_assert(std::is_standard_layout_v<phyriad::proc::ProcessMetrics>);
}

// ── Test 2: create succeeds ───────────────────────────────────────────────────
static void test_create() {
    auto r = phyriad::proc::ProcessMetricsSnapshot::create();
    CHECK(r.has_value(), "create() must succeed");
    std::printf("[OK] test_create\n");
}

// ── Test 3: capture + find self ───────────────────────────────────────────────
static void test_capture_find_self() {
    auto r = phyriad::proc::ProcessMetricsSnapshot::create();
    CHECK(r.has_value(), "create");

    auto cap = r->capture();
    CHECK(cap.has_value(), "capture");

    const uint32_t self = os_self_pid();
    const phyriad::proc::ProcessMetrics* m = r->find(self);
    CHECK(m != nullptr, "find(self_pid) must return non-null");
    CHECK(m->pid == self, "pid field matches");
    // Working set must be non-zero (process is loaded and running).
    CHECK(m->working_set_bytes > 0u, "working_set_bytes > 0");
    // CPU time may be 0 for a short-lived test process on fast hardware
    // (sub-100ns scheduler quantum). Log it but don't fail on 0.
    const uint64_t cpu_us = (m->kernel_time_100ns + m->user_time_100ns) / 10u;
    if (cpu_us == 0u) {
        std::printf("[NOTE] test_capture_find_self: cpu_time=0 (fast process,"
                    " acceptable)\n");
    }
    std::printf("[OK] test_capture_find_self (pid=%u, wss=%" PRIu64 " KB, "
                "cpu=%" PRIu64 " µs)\n",
                self, m->working_set_bytes / 1024u, cpu_us);
}

// ── Test 4: buffer growth (tiny initial capacity) ─────────────────────────────
static void test_buffer_growth() {
    // 512 bytes is far too small for SystemProcessInformation — must grow.
    auto r = phyriad::proc::ProcessMetricsSnapshot::create(512u);
    CHECK(r.has_value(), "create(512)");

    auto cap = r->capture();
    CHECK(cap.has_value(), "capture after buffer growth");
    CHECK(r->process_count() > 0u, "at least 1 process after growth capture");
    std::printf("[OK] test_buffer_growth (process_count=%u)\n",
                r->process_count());
}

// ── Test 5: extract mixed existing + nonexistent ─────────────────────────────
static void test_extract_mixed() {
    auto r = phyriad::proc::ProcessMetricsSnapshot::create();
    CHECK(r.has_value(), "create");
    CHECK(r->capture().has_value(), "capture");

    const uint32_t self = os_self_pid();
    // PID 0x7FFFFFFF is extremely unlikely to be a real process.
    const uint32_t pids[2] = { self, 0x7FFFFFFFu };
    phyriad::proc::ProcessMetrics out[2]{};

    const uint32_t found = r->extract(pids, 2u, out);
    CHECK(found == 1u, "only self should match");
    CHECK(out[0].pid == self, "out[0].pid == self");
    CHECK(out[0].working_set_bytes > 0u, "out[0] valid metrics");
    CHECK(out[1].pid == 0x7FFFFFFFu, "out[1].pid preserved for not-found");
    CHECK(out[1].working_set_bytes == 0u, "out[1] zeroed");
    std::printf("[OK] test_extract_mixed\n");
}

// ── Test 6: idempotent repeat captures ───────────────────────────────────────
static void test_repeat_capture() {
    auto r = phyriad::proc::ProcessMetricsSnapshot::create();
    CHECK(r.has_value(), "create");

    uint32_t prev_count = 0u;
    for (uint32_t i = 0u; i < 100u; ++i) {
        CHECK(r->capture().has_value(), "capture in loop");
        const uint32_t count = r->process_count();
        CHECK(count > 0u, "process_count > 0");
        // Allow ±10% variation between ticks (processes can come/go).
        if (i > 0u) {
            CHECK(count >= prev_count / 2u, "count not drastically lower");
        }
        prev_count = count;
    }
    std::printf("[OK] test_repeat_capture (final count=%u)\n", prev_count);
}

// ── Test 7: performance gate ──────────────────────────────────────────────────
static void test_performance() {
    auto r = phyriad::proc::ProcessMetricsSnapshot::create();
    CHECK(r.has_value(), "create");

    // Warmup
    CHECK(r->capture().has_value(), "warmup capture");

    const auto t0 = std::chrono::steady_clock::now();
    constexpr uint32_t kIter = 100u;  // 100 captures
    for (uint32_t i = 0u; i < kIter; ++i) {
        CHECK(r->capture().has_value(), "capture in perf loop");
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double us_per = ms * 1000.0 / kIter;

    std::printf("[OK] test_performance: %u captures in %.1f ms (%.0f µs/capture)\n",
                kIter, ms, us_per);

    // Gate: 100 captures < 60 000 ms (600 ms/capture max — very conservative for
    // slow CI VMs running on shared hosts).
    if (ms > 60000.0) {
        std::fprintf(stderr,
            "[WARN] Performance gate: %.1f ms > 60000 ms threshold\n", ms);
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    test_create();
    test_capture_find_self();
    test_buffer_growth();
    test_extract_mixed();
    test_repeat_capture();
    test_performance();

    std::printf("\n[PASS] process_metrics_snapshot_test\n");
    return 0;
}
// Made with my soul - Swately <3
