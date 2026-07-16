// framework/process/tests/enumerate_threads_test.cpp
// Tests for GFR-Ayama-3: ProcessMetricsSnapshot::extract_threads /
// thread_count_for. Uses the current process (and spawns extra worker
// threads) as the test target — no admin or external process needed.

#include <phyriad/process/ProcessMetricsSnapshot.hpp>
#include <phyriad/process/CurrentProcess.hpp>
#include <phyriad/schema/Error.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include <phyriad/hal/MemoryOrder.hpp>

static int failures = 0;
#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                     \
        std::fprintf(stderr,                                           \
            "[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        ++failures;                                                    \
    } else {                                                           \
        std::fprintf(stdout, "[OK]   %s\n", #cond);                    \
    }                                                                  \
} while (0)

static void test_pod_layout() {
    std::fprintf(stdout,
        "\n── §1 ThreadEntry POD layout ──────────────────────────────\n");
    EXPECT(sizeof(phyriad::proc::ThreadEntry) == 48u);
    EXPECT(alignof(phyriad::proc::ThreadEntry) == 8u);
    EXPECT(std::is_trivially_copyable_v<phyriad::proc::ThreadEntry>);
    EXPECT(std::is_standard_layout_v<phyriad::proc::ThreadEntry>);
}

static void test_basic_capture_self() {
    std::fprintf(stdout,
        "\n── §2 capture + thread_count_for(self) > 0 ────────────────\n");

    auto sr = phyriad::proc::ProcessMetricsSnapshot::create();
    EXPECT(sr.has_value());
    if (!sr) return;
    auto& snap = *sr;

    auto cap = snap.capture();
    EXPECT(cap.has_value());

    const uint32_t self_pid = phyriad::proc::self_pid();
    const uint32_t count = snap.thread_count_for(self_pid);
    EXPECT(count >= 1u);  // at least 1 thread (this one)
    std::fprintf(stdout, "       self pid=%u, threads=%u\n", self_pid, count);
}

static void test_extract_self_threads() {
    std::fprintf(stdout,
        "\n── §3 extract_threads(self) returns valid TIDs ────────────\n");

    auto sr = phyriad::proc::ProcessMetricsSnapshot::create();
    if (!sr) return;
    auto& snap = *sr;

    // Spawn some workers so we have known thread count.
    constexpr uint32_t kWorkers = 5u;
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (uint32_t i = 0u; i < kWorkers; ++i) {
        workers.emplace_back([&stop]() {
            while (!phyriad::hal::seq_load_acquire(stop)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
    // Give threads time to enter the wait loop.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto cap = snap.capture();
    EXPECT(cap.has_value());

    const uint32_t self_pid = phyriad::proc::self_pid();
    const uint32_t n_threads = snap.thread_count_for(self_pid);
    EXPECT(n_threads >= kWorkers + 1u);  // workers + main

    // Extract up to 64 threads.
    phyriad::proc::ThreadEntry entries[64]{};
    const uint32_t got = snap.extract_threads(self_pid, entries, 64u);
    EXPECT(got >= kWorkers + 1u);
    EXPECT(got <= 64u);
    std::fprintf(stdout, "       extracted %u threads\n", got);

    // Validate each entry: tid != 0, pid matches.
    bool all_valid = true;
    for (uint32_t i = 0u; i < got; ++i) {
        if (entries[i].tid == 0u || entries[i].pid != self_pid) {
            all_valid = false;
            std::fprintf(stderr,
                "[FAIL]  entry %u: tid=%u pid=%u (expected pid=%u)\n",
                i, entries[i].tid, entries[i].pid, self_pid);
        }
    }
    EXPECT(all_valid);

    // No duplicate TIDs in the result.
    bool no_dupes = true;
    for (uint32_t i = 0u; i < got && no_dupes; ++i) {
        for (uint32_t j = i + 1u; j < got; ++j) {
            if (entries[i].tid == entries[j].tid) {
                no_dupes = false;
                std::fprintf(stderr,
                    "[FAIL]  duplicate TID %u at indices %u and %u\n",
                    entries[i].tid, i, j);
                break;
            }
        }
    }
    EXPECT(no_dupes);

    // Cleanup.
    phyriad::hal::seq_store_release(stop, true);
    for (auto& w : workers) w.join();
}

static void test_extract_unknown_pid() {
    std::fprintf(stdout,
        "\n── §4 thread_count_for / extract_threads on unknown PID ────\n");

    auto sr = phyriad::proc::ProcessMetricsSnapshot::create();
    if (!sr) return;
    auto& snap = *sr;
    (void)snap.capture();

    const uint32_t bogus_pid = 0xFFFFFFFEu;
    EXPECT(snap.thread_count_for(bogus_pid) == 0u);

    phyriad::proc::ThreadEntry entries[8]{};
    EXPECT(snap.extract_threads(bogus_pid, entries, 8u) == 0u);
}

static void test_buffer_truncation() {
    std::fprintf(stdout,
        "\n── §5 extract_threads respects max_count ──────────────────\n");

    auto sr = phyriad::proc::ProcessMetricsSnapshot::create();
    if (!sr) return;
    auto& snap = *sr;
    (void)snap.capture();

    const uint32_t self_pid = phyriad::proc::self_pid();
    const uint32_t total = snap.thread_count_for(self_pid);
    if (total < 2u) {
        std::fprintf(stdout, "       skipped — only %u threads in test process\n", total);
        return;
    }

    // Request only 1 entry from a multi-thread process.
    phyriad::proc::ThreadEntry entries[1]{};
    const uint32_t got = snap.extract_threads(self_pid, entries, 1u);
    EXPECT(got == 1u);
    EXPECT(entries[0].pid == self_pid);
}

static void test_thread_times_monotonic() {
    std::fprintf(stdout,
        "\n── §6 thread times monotonically increase across captures ─\n");

    auto sr = phyriad::proc::ProcessMetricsSnapshot::create();
    if (!sr) return;
    auto& snap = *sr;

    const uint32_t self_pid = phyriad::proc::self_pid();

    // First capture.
    (void)snap.capture();
    phyriad::proc::ThreadEntry t1[8]{};
    const uint32_t n1 = snap.extract_threads(self_pid, t1, 8u);
    if (n1 == 0u) return;

    // Burn some CPU so kernel/user times advance.
    volatile uint64_t spin = 0u;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline) {
        for (uint32_t i = 0u; i < 1000u; ++i) ++spin;
    }

    // Second capture.
    (void)snap.capture();
    phyriad::proc::ThreadEntry t2[8]{};
    const uint32_t n2 = snap.extract_threads(self_pid, t2, 8u);
    EXPECT(n2 >= 1u);

    // Find the matching TID in both captures (main thread is TID-stable).
    // At least one thread should show non-decreasing time.
    bool any_progressed = false;
    for (uint32_t i = 0u; i < n2 && !any_progressed; ++i) {
        for (uint32_t j = 0u; j < n1; ++j) {
            if (t1[j].tid == t2[i].tid) {
                const uint64_t t_before = t1[j].user_time_100ns + t1[j].kernel_time_100ns;
                const uint64_t t_after  = t2[i].user_time_100ns + t2[i].kernel_time_100ns;
                if (t_after > t_before) any_progressed = true;
                break;
            }
        }
    }
    EXPECT(any_progressed);
}

int main() {
    std::fprintf(stdout,
        "═══════════════════════════════════════════════════════════════\n"
        "  GFR-Ayama-3 — extract_threads / thread_count_for tests\n"
        "═══════════════════════════════════════════════════════════════\n");

    test_pod_layout();
    test_basic_capture_self();
    test_extract_self_threads();
    test_extract_unknown_pid();
    test_buffer_truncation();
    test_thread_times_monotonic();

    std::fprintf(stdout, "\n────────────────────────────────────────────────\n");
    if (failures == 0) {
        std::fprintf(stdout, "[PASS] enumerate_threads_test — all checks passed\n");
        return 0;
    } else {
        std::fprintf(stderr,
            "[FAIL] enumerate_threads_test — %d failures\n", failures);
        return 1;
    }
}
// Made with my soul - Swately <3
