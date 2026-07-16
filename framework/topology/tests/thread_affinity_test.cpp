// framework/topology/tests/thread_affinity_test.cpp
// Tests for GFR-Ayama-2 (set_thread_affinity / get_thread_affinity) and
// GFR-Ayama-4 (set_thread_ideal_processor / get_thread_ideal_processor).
//
// Test strategy:
//   - Run on the current thread (self-TID) which we always have access to.
//   - Cross-process tests are deferred to downstream integration tests
//     (e.g. apps/ayama/) where admin elevation is verified upstream.

#include <phyriad/topology/HardwareTopology.hpp>
#include <phyriad/schema/Error.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <phyriad/hal/MemoryOrder.hpp>

#ifdef _WIN32
#  include <windows.h>
#  include <processthreadsapi.h>
inline uint32_t current_tid() noexcept {
    return static_cast<uint32_t>(GetCurrentThreadId());
}
#else
#  include <sys/syscall.h>
#  include <unistd.h>
inline uint32_t current_tid() noexcept {
    return static_cast<uint32_t>(syscall(SYS_gettid));
}
#endif

static int failures = 0;
#define EXPECT(cond) do {                                                  \
    if (!(cond)) {                                                         \
        std::fprintf(stderr,                                               \
            "[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
        ++failures;                                                        \
    } else {                                                               \
        std::fprintf(stdout, "[OK]   %s\n", #cond);                        \
    }                                                                      \
} while (0)

static void test_get_thread_affinity_self() {
    std::fprintf(stdout,
        "\n── §1.1 get_thread_affinity on self thread ────────────────\n");

    const uint32_t tid = current_tid();
    auto r = phyriad::hw::get_thread_affinity(tid);
    EXPECT(r.has_value());
    if (r) {
        EXPECT(*r != 0ull);  // self thread always has at least 1 core mask
        std::fprintf(stdout, "       self mask = 0x%llx\n",
            static_cast<unsigned long long>(*r));
    }
}

static void test_set_thread_affinity_roundtrip() {
    std::fprintf(stdout,
        "\n── §1.2 set_thread_affinity roundtrip on self thread ───────\n");

    const uint32_t tid = current_tid();
    // Read original
    auto orig_r = phyriad::hw::get_thread_affinity(tid);
    assert(orig_r.has_value());
    const uint64_t orig_mask = *orig_r;

    // Set to single-bit (bit 0 — always exists in any system)
    auto set_r = phyriad::hw::set_thread_affinity(tid, 1ull);
    EXPECT(set_r.has_value());
    if (set_r) {
        EXPECT(*set_r == orig_mask);  // returned prev == original
    }

    // Verify the new mask is what we set
    auto check_r = phyriad::hw::get_thread_affinity(tid);
    EXPECT(check_r.has_value());
    if (check_r) {
        EXPECT(*check_r == 1ull);
    }

    // Restore
    auto restore_r = phyriad::hw::set_thread_affinity(tid, orig_mask);
    EXPECT(restore_r.has_value());
    if (restore_r) {
        EXPECT(*restore_r == 1ull);  // prev was the single-bit we set
    }

    // Verify restored
    auto final_r = phyriad::hw::get_thread_affinity(tid);
    EXPECT(final_r.has_value());
    if (final_r) {
        EXPECT(*final_r == orig_mask);
    }
}

static void test_set_thread_affinity_rejects_zero() {
    std::fprintf(stdout,
        "\n── §1.3 set_thread_affinity rejects mask == 0 ──────────────\n");

    const uint32_t tid = current_tid();
    auto r = phyriad::hw::set_thread_affinity(tid, 0ull);
    EXPECT(!r.has_value());
    if (!r) {
        EXPECT(r.error().code == phyriad::ErrorCode::InvalidArgument);
    }
}

static void test_set_thread_affinity_invalid_tid() {
    std::fprintf(stdout,
        "\n── §1.4 set_thread_affinity rejects invalid TID ────────────\n");

    // 0xFFFFFFFE is essentially guaranteed to not be a real TID.
    auto r = phyriad::hw::set_thread_affinity(0xFFFFFFFEu, 1ull);
    EXPECT(!r.has_value());
    if (!r) {
        // Either InvalidArgument (OpenThread failed) or PermissionDenied —
        // both acceptable; just verify it didn't pretend to succeed.
        EXPECT(r.error().code == phyriad::ErrorCode::InvalidArgument ||
               r.error().code == phyriad::ErrorCode::PermissionDenied);
    }
}

static void test_set_thread_affinity_other_thread() {
    std::fprintf(stdout,
        "\n── §1.5 set_thread_affinity on a spawned worker thread ─────\n");

    std::atomic<uint32_t> worker_tid{0u};
    std::atomic<bool>     worker_ready{false};
    std::atomic<bool>     worker_stop{false};

    std::thread worker([&]() {
        phyriad::hal::seq_store_release(worker_tid, current_tid());
        phyriad::hal::seq_store_release(worker_ready, true);
        while (!phyriad::hal::seq_load_acquire(worker_stop)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Wait for worker to publish its TID.
    while (!phyriad::hal::seq_load_acquire(worker_ready)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const uint32_t wtid = phyriad::hal::seq_load_acquire(worker_tid);
    EXPECT(wtid != 0u);

    // Pin worker to core 0.
    auto pin_r = phyriad::hw::set_thread_affinity(wtid, 1ull);
    EXPECT(pin_r.has_value());

    // Verify.
    auto check_r = phyriad::hw::get_thread_affinity(wtid);
    EXPECT(check_r.has_value() && check_r.value() == 1ull);

    // Cleanup: restore (best-effort) and join.
    if (pin_r.has_value()) {
        (void)phyriad::hw::set_thread_affinity(wtid, *pin_r);
    }
    phyriad::hal::seq_store_release(worker_stop, true);
    worker.join();
}

static void test_ideal_processor_self() {
    std::fprintf(stdout,
        "\n── §2.1 set_thread_ideal_processor on self thread ──────────\n");

    const uint32_t tid = current_tid();

    // On non-Windows this returns Unavailable — accept that path.
    auto orig_r = phyriad::hw::get_thread_ideal_processor(tid);
#ifdef _WIN32
    EXPECT(orig_r.has_value());
    if (!orig_r) return;
    const uint32_t orig_ideal = *orig_r;
    std::fprintf(stdout, "       self ideal proc = %u\n", orig_ideal);

    // Set ideal to core 0.
    auto set_r = phyriad::hw::set_thread_ideal_processor(tid, 0u);
    EXPECT(set_r.has_value());

    // Verify (best-effort — read may or may not reflect immediately).
    auto check_r = phyriad::hw::get_thread_ideal_processor(tid);
    EXPECT(check_r.has_value());

    // Restore.
    (void)phyriad::hw::set_thread_ideal_processor(tid, orig_ideal);
#else
    EXPECT(!orig_r.has_value());
    if (!orig_r) {
        EXPECT(orig_r.error().code == phyriad::ErrorCode::Unavailable);
    }
#endif
}

static void test_ideal_processor_rejects_out_of_range() {
    std::fprintf(stdout,
        "\n── §2.2 set_thread_ideal_processor rejects out-of-range id ─\n");

    const uint32_t tid = current_tid();
    // 4096 logical CPUs is well above anything realistic; reject.
    auto r = phyriad::hw::set_thread_ideal_processor(tid, 99999u);
    EXPECT(!r.has_value());
    if (!r) {
        // InvalidArgument on Windows (range check), Unavailable on POSIX.
        EXPECT(r.error().code == phyriad::ErrorCode::InvalidArgument ||
               r.error().code == phyriad::ErrorCode::Unavailable);
    }
}

int main() {
    std::fprintf(stdout,
        "═══════════════════════════════════════════════════════════════\n"
        "  GFR-Ayama-2 / -4 — thread affinity + ideal processor tests\n"
        "═══════════════════════════════════════════════════════════════\n");

    test_get_thread_affinity_self();
    test_set_thread_affinity_roundtrip();
    test_set_thread_affinity_rejects_zero();
    test_set_thread_affinity_invalid_tid();
    test_set_thread_affinity_other_thread();
    test_ideal_processor_self();
    test_ideal_processor_rejects_out_of_range();

    std::fprintf(stdout, "\n────────────────────────────────────────────────\n");
    if (failures == 0) {
        std::fprintf(stdout, "[PASS] thread_affinity_test — all checks passed\n");
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] thread_affinity_test — %d failures\n",
            failures);
        return 1;
    }
}
// Made with my soul - Swately <3
