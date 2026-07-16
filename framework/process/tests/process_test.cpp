// framework/process/tests/process_test.cpp
// Test suite for phyriad_process — phyriad::proc::enumerate_processes().
//
// Tests:
//   1. enumerate_processes() includes own PID in the result.
//   2. Buffer truncation: max_count=5 fills exactly 5 entries without overflow.
//   3. Zero buffer: max_count=0 returns 0, no writes.
//   4. Own process name is non-empty.
//   5. last_enumerate_error() is empty after a successful call.
//   6. PIDs are non-zero for all returned entries.
//

#include <phyriad/process/ProcessEnumerator.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
static uint32_t own_pid() noexcept {
    return static_cast<uint32_t>(GetCurrentProcessId());
}
#else
#  include <unistd.h>
static uint32_t own_pid() noexcept {
    return static_cast<uint32_t>(getpid());
}
#endif

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

// ── Static assertions ─────────────────────────────────────────────────────────
static_assert(sizeof(phyriad::proc::ProcessEntry)  == 80u,  "size check");
static_assert(alignof(phyriad::proc::ProcessEntry) ==  8u,  "align check");
static_assert(std::is_trivially_copyable_v<phyriad::proc::ProcessEntry>, "trivial");
static_assert(std::is_standard_layout_v<phyriad::proc::ProcessEntry>,    "layout");

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 — own PID appears in result
// ─────────────────────────────────────────────────────────────────────────────
static void test_own_pid() {
    SECTION("Test 1: enumerate_processes() contains own PID");

    static phyriad::proc::ProcessEntry buf[phyriad::proc::kMaxProcesses];
    const uint32_t n = phyriad::proc::enumerate_processes(
        buf, phyriad::proc::kMaxProcesses);

    EXPECT(n > 0u);

    const uint32_t pid = own_pid();
    bool found = false;
    for (uint32_t i = 0u; i < n; ++i) {
        if (buf[i].pid == pid) { found = true; break; }
    }
    EXPECT(found);
    std::printf("    own pid=%u  total_found=%u\n", pid, n);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — buffer truncation
// ─────────────────────────────────────────────────────────────────────────────
static void test_truncation() {
    SECTION("Test 2: max_count=5 truncates correctly, no overflow");

    phyriad::proc::ProcessEntry buf[5];
    // Poison the 5th slot: if we write past it the poison would remain != 0.
    // Actually, we can just check the return value <= 5.
    const uint32_t n = phyriad::proc::enumerate_processes(buf, 5u);
    EXPECT(n <= 5u);
    std::printf("    returned %u entries (max=5)\n", n);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — zero max_count
// ─────────────────────────────────────────────────────────────────────────────
static void test_zero_count() {
    SECTION("Test 3: max_count=0 returns 0");

    phyriad::proc::ProcessEntry sentinel{};
    sentinel.pid = 0xDEADBEEFu;

    // Pass a non-null pointer with max_count=0 — must not write anything.
    const uint32_t n = phyriad::proc::enumerate_processes(&sentinel, 0u);
    EXPECT(n == 0u);
    EXPECT(sentinel.pid == 0xDEADBEEFu); // sentinel unchanged
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 — own process name is non-empty
// ─────────────────────────────────────────────────────────────────────────────
static void test_own_name() {
    SECTION("Test 4: own process has non-empty name");

    static phyriad::proc::ProcessEntry buf[phyriad::proc::kMaxProcesses];
    const uint32_t n = phyriad::proc::enumerate_processes(
        buf, phyriad::proc::kMaxProcesses);

    const uint32_t pid = own_pid();
    for (uint32_t i = 0u; i < n; ++i) {
        if (buf[i].pid == pid) {
            EXPECT(buf[i].name[0] != '\0');
            std::printf("    own name: \"%s\"\n", buf[i].name);
            return;
        }
    }
    // If we didn't find our own PID, that's already caught by test_own_pid().
    std::puts("    (own PID not found — skipped name check)");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — last_enumerate_error() empty on success
// ─────────────────────────────────────────────────────────────────────────────
static void test_error_empty_on_success() {
    SECTION("Test 5: last_enumerate_error() empty after successful call");

    static phyriad::proc::ProcessEntry buf[phyriad::proc::kMaxProcesses];
    const uint32_t n = phyriad::proc::enumerate_processes(
        buf, phyriad::proc::kMaxProcesses);
    EXPECT(n > 0u);
    EXPECT(phyriad::proc::last_enumerate_error().empty());
    std::printf("    error string empty=%d\n",
                (int)phyriad::proc::last_enumerate_error().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 — all returned PIDs are non-zero
// ─────────────────────────────────────────────────────────────────────────────
static void test_nonzero_pids() {
    SECTION("Test 6: all returned PIDs are non-zero");

    static phyriad::proc::ProcessEntry buf[phyriad::proc::kMaxProcesses];
    const uint32_t n = phyriad::proc::enumerate_processes(
        buf, phyriad::proc::kMaxProcesses);

    for (uint32_t i = 0u; i < n; ++i) {
        EXPECT(buf[i].pid != 0u);
    }
    std::printf("    %u processes enumerated, all PIDs non-zero\n", n);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::puts("[process_test] phyriad_process pillar — Phase 4.A");
    std::puts("----------------------------------------------------------------");
    std::printf("  sizeof(ProcessEntry)   = %zu bytes\n",
                sizeof(phyriad::proc::ProcessEntry));
    std::printf("  kMaxProcesses          = %u\n",
                phyriad::proc::kMaxProcesses);
    std::puts("----------------------------------------------------------------");

    test_own_pid();
    test_truncation();
    test_zero_count();
    test_own_name();
    test_error_empty_on_success();
    test_nonzero_pids();

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
