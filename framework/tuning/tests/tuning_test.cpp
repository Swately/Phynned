// framework/tuning/tests/tuning_test.cpp
// Tuning pillar test — Phase 1.D
//
// Sections:
//   §1   PrivilegeCheck::probe() — returns a PrivilegeInfo (no crash)
//   §2   PrivilegeLevel enum values
//   §3   PrivilegeInfo fields are consistent (can_set_rt implies level >= Partial)
//   §4   WindowsTuner::apply() — either succeeds or fails gracefully
//   §5   WindowsTuner::revert() — idempotent
//   §6   WindowsTuner::set_thread_realtime() — callable, returns bool
//   §7   LinuxTuner on Windows — stub always returns false
//   §8   TuningDaemon start + stop (with WindowsTuner)
//   §9   TuningDaemon reapply_count() starts at 0
//   §10  TuningDaemon stops cleanly without deadlock
//
#include <phyriad/tuning/TuningAll.hpp>
#include <chrono>
#include <cstdio>
#include <thread>

namespace tn = phyriad::tuning;

// ── Minimal test harness ──────────────────────────────────────────────────────
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

// ── §1 PrivilegeCheck::probe() ────────────────────────────────────────────────
static void test_privilege_probe() {
    SECTION("Test 1: PrivilegeCheck::probe() — returns a valid PrivilegeInfo");

    auto info = tn::PrivilegeCheck::probe();

    // Level is one of the four enum values (not garbage).
    EXPECT(info.level == tn::PrivilegeLevel::None     ||
           info.level == tn::PrivilegeLevel::Partial  ||
           info.level == tn::PrivilegeLevel::Elevated ||
           info.level == tn::PrivilegeLevel::Admin);

    // On any sane OS, affinity is available.
    EXPECT(info.can_set_affinity == true);

    std::printf("    level=%d  rt=%d  lock=%d  timer=%d\n",
        (int)info.level,
        (int)info.can_set_rt_prio,
        (int)info.can_lock_pages,
        (int)info.can_raise_timer_resolution);
}

// ── §2 PrivilegeLevel enum values ─────────────────────────────────────────────
// Compile-time check: use static_assert to avoid C4127 on MSVC.
static_assert((uint8_t)tn::PrivilegeLevel::None     == 0u);
static_assert((uint8_t)tn::PrivilegeLevel::Partial  == 1u);
static_assert((uint8_t)tn::PrivilegeLevel::Elevated == 2u);
static_assert((uint8_t)tn::PrivilegeLevel::Admin    == 3u);

static void test_privilege_enum() {
    SECTION("Test 2: PrivilegeLevel enum values (static_asserts at compile time)");
    ++g_pass; ++g_pass; ++g_pass; ++g_pass;  // 4 assertions already verified above
}

// ── §3 PrivilegeInfo consistency ──────────────────────────────────────────────
static void test_privilege_consistency() {
    SECTION("Test 3: PrivilegeInfo — can_set_rt_prio implies level >= Partial");

    auto info = tn::PrivilegeCheck::probe();
    // Forward direction only: if RT scheduling is available, the privilege
    // level must be at least Partial. The reverse direction does NOT hold
    // (an Admin context can still lack SeIncreaseBasePriorityPrivilege if
    // group policy or the parent process explicitly disabled it).
    if (info.can_set_rt_prio) {
        EXPECT(info.level >= tn::PrivilegeLevel::Partial);
    }
    // No assertion when !can_set_rt_prio — any level is valid (the privilege
    // is simply not enabled in the current token regardless of group).
}

// ── §4 WindowsTuner::apply() ──────────────────────────────────────────────────
static void test_windows_tuner_apply() {
    SECTION("Test 4: WindowsTuner::apply() — applies or gracefully degrades");

    tn::WindowsTuner tuner{};
    const bool applied = tuner.apply();

    // apply() should not crash regardless of privilege.
    // It may return false if we lack privilege (CI environment).
    (void)applied;
    std::printf("    WindowsTuner::apply() returned %s\n",
        applied ? "true" : "false");
    EXPECT(true);  // just checks it didn't crash
}

// ── §5 WindowsTuner::revert() idempotent ─────────────────────────────────────
static void test_windows_tuner_revert() {
    SECTION("Test 5: WindowsTuner::revert() — idempotent");

    tn::WindowsTuner tuner{};
    tuner.apply();
    tuner.revert();
    tuner.revert();  // double revert — must not crash
    EXPECT(!tuner.is_applied());
}

// ── §6 WindowsTuner::set_thread_realtime() ───────────────────────────────────
static void test_windows_tuner_thread_rt() {
    SECTION("Test 6: WindowsTuner::set_thread_realtime() — callable");

    // Returns true on privileged Windows, false on unprivileged.
    const bool ok = tn::WindowsTuner::set_thread_realtime();
    (void)ok;
    std::printf("    set_thread_realtime() returned %s\n",
        ok ? "true" : "false");
    EXPECT(true);  // just checks no crash
}

// ── §7 LinuxTuner on Windows is a stub ────────────────────────────────────────
static void test_linux_tuner_stub() {
    SECTION("Test 7: LinuxTuner — stub on Windows always returns false");

    tn::LinuxTuner tuner{};
#ifdef _WIN32
    EXPECT(!tuner.apply());
    EXPECT(!tuner.is_applied());
#else
    // On Linux, apply() may succeed or gracefully fail.
    (void)tuner.apply();
    EXPECT(true);
#endif
}

// ── §8 TuningDaemon start + stop ─────────────────────────────────────────────
static void test_tuning_daemon_start_stop() {
    SECTION("Test 8: TuningDaemon start + stop (WindowsTuner)");

    tn::WindowsTuner tuner{};
    tn::TuningDaemon daemon{};

    EXPECT(!daemon.running());
    daemon.start(tuner, 100u);  // 100 ms poll
    EXPECT(daemon.running());

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    daemon.stop();
    EXPECT(!daemon.running());
}

// ── §9 TuningDaemon reapply_count ────────────────────────────────────────────
static void test_tuning_daemon_reapply_count() {
    SECTION("Test 9: TuningDaemon reapply_count() starts at 0");

    tn::WindowsTuner tuner{};
    tn::TuningDaemon daemon{};
    EXPECT(daemon.reapply_count() == 0u);

    daemon.start(tuner, 10'000u);  // very long poll — won't fire
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT(daemon.reapply_count() == 0u);  // hasn't detected a loss
    daemon.stop();
}

// ── §10 TuningDaemon clean stop ───────────────────────────────────────────────
static void test_tuning_daemon_clean_stop() {
    SECTION("Test 10: TuningDaemon stops cleanly without deadlock");

    tn::WindowsTuner tuner{};
    tuner.apply();  // pre-apply so daemon won't trigger a reapply

    tn::TuningDaemon daemon{};
    daemon.start(tuner, 50u);  // 50 ms poll

    // Stop while daemon is mid-sleep — should join cleanly.
    daemon.stop();
    EXPECT(!daemon.running());
    EXPECT(true);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::printf("[tuning_test] phyriad_tuning pillar\n");
    std::printf("----------------------------------------------------------------\n");

    test_privilege_probe();
    test_privilege_enum();
    test_privilege_consistency();
    test_windows_tuner_apply();
    test_windows_tuner_revert();
    test_windows_tuner_thread_rt();
    test_linux_tuner_stub();
    test_tuning_daemon_start_stop();
    test_tuning_daemon_reapply_count();
    test_tuning_daemon_clean_stop();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d tests passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d tests FAILED\n", g_fail, total);

    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
