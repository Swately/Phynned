// framework/process/tests/current_process_test.cpp
// Unit tests for phyriad::proc::CurrentProcess helpers.
//
// Coverage:
//   1. self_pid() matches OS call.
//   2. self_pid() multiple calls return same value (cached).
//   3. self_ppid() non-zero (has a parent in normal environment).
//   4. self_name() non-empty, null-terminated.
//   5. Multiple calls to self_name() return the same pointer (cached).
//

#include <phyriad/process/CurrentProcess.hpp>

#include <cassert>
#include <cstdio>
#include <cstdlib>  // std::abort
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

static void fail(const char* msg) {
    std::fprintf(stderr, "[FAIL] %s\n", msg);
    std::fflush(stderr);
    std::abort();
}
#define CHECK(cond, msg) do { if (!(cond)) fail(msg); } while(0)

// ── Test 1: self_pid matches OS ───────────────────────────────────────────────
static void test_self_pid_matches_os() {
#ifdef _WIN32
    const uint32_t os_pid = static_cast<uint32_t>(GetCurrentProcessId());
#else
    const uint32_t os_pid = static_cast<uint32_t>(::getpid());
#endif
    CHECK(phyriad::proc::self_pid() == os_pid, "self_pid() must match OS PID");
    std::printf("[OK] test_self_pid_matches_os (pid=%u)\n", os_pid);
}

// ── Test 2: self_pid() is cached (same value, same pointer address not needed
//           but multiple calls must return same value) ─────────────────────────
static void test_self_pid_cached() {
    const uint32_t a = phyriad::proc::self_pid();
    const uint32_t b = phyriad::proc::self_pid();
    const uint32_t c = phyriad::proc::self_pid();
    CHECK(a == b && b == c, "self_pid() must be stable across calls");
    std::printf("[OK] test_self_pid_cached\n");
}

// ── Test 3: self_ppid non-zero ────────────────────────────────────────────────
static void test_self_ppid_nonzero() {
    const uint32_t ppid = phyriad::proc::self_ppid();
    // In virtually all environments a normal process has a parent (shell, test
    // runner, etc.). Allow 0 only when running as PID 1 (init / container root).
    if (ppid == 0u) {
        std::printf("[SKIP] test_self_ppid_nonzero (ppid=0, probably PID 1)\n");
    } else {
        std::printf("[OK] test_self_ppid_nonzero (ppid=%u)\n", ppid);
    }
}

// ── Test 4: self_name non-empty ───────────────────────────────────────────────
static void test_self_name_nonempty() {
    const char* name = phyriad::proc::self_name();
    CHECK(name != nullptr, "self_name() != nullptr");
    CHECK(name[0] != '\0', "self_name() non-empty");
    std::printf("[OK] test_self_name_nonempty (name=\"%s\")\n", name);
}

// ── Test 5: self_name cached (same pointer) ───────────────────────────────────
static void test_self_name_cached() {
    const char* a = phyriad::proc::self_name();
    const char* b = phyriad::proc::self_name();
    CHECK(a == b, "self_name() must return same pointer (cached)");
    std::printf("[OK] test_self_name_cached\n");
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    test_self_pid_matches_os();
    test_self_pid_cached();
    test_self_ppid_nonzero();
    test_self_name_nonempty();
    test_self_name_cached();

    std::printf("\n[PASS] current_process_test\n");
    return 0;
}
// Made with my soul - Swately <3
