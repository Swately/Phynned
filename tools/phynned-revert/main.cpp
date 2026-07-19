// tools/phynned-revert/main.cpp
// phynned-revert — the external crash-recovery DEAD-MAN for DR1.
//
// Affinity/CPU-Set placements are properties of the TARGET process and PERSIST
// after the agent (the setter) dies. `~ActionExecutor -> revert_all` CANNOT run
// on `taskkill /f`, so a mass run that was killed orphans every placement it made.
// This standalone tool is the recovery path that covers kill-WITHOUT-restart: the
// operator (or a watchdog/service supervisor) runs it, it reads the write-ahead
// RevertJournal, and reverts every surviving APPLIED placement to its CAPTURED
// prev_mask — reconciled against the live process by the pid-recycle guard.
//
// It is safe to run anytime and is IDEMPOTENT: it marks each record REVERTED as
// it processes it, so a second run finds nothing to do (recover() only returns
// PENDING+APPLIED). A record whose process is gone / pid-recycled is dropped
// (marked REVERTED) without touching any live process.
//
// Usage:
//   phynned-revert [--journal PATH]     revert survivors in PATH (default:
//                                       %LOCALAPPDATA%\Phynned\revert_journal)
//   phynned-revert --dry-run [...]      report survivors, change nothing
//   phynned-revert --self-test          end-to-end proof: spawn a suspended
//                                       child, narrow its affinity, journal it,
//                                       then prove one revert restores it and a
//                                       second pass is a no-op (idempotent).
//
#include <phynned/action/RevertJournal.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

using phynned::action::RevertJournal;
using phynned::action::RevertKey;
using phynned::action::RevertRecord;
using phynned::action::RevertStatus;
using phynned::action::to_string;

// The least-privilege rights the revert needs: SET to write the mask, QUERY to
// read it back for the proof. Never a cheat-shaped handle (SR1).
static constexpr DWORD kRevertAccess =
    PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION; // 0x1200

// Restore `pid`'s affinity to `prev_mask`. Returns true if the Set succeeded.
static bool revert_to_prev(uint32_t pid, uint64_t prev_mask) {
    if (prev_mask == 0ull) return false; // nothing captured — cannot restore
    const HANDLE h = OpenProcess(kRevertAccess, FALSE, pid);
    if (h == nullptr) return false;
    const BOOL ok = SetProcessAffinityMask(h, static_cast<DWORD_PTR>(prev_mask));
    CloseHandle(h);
    return ok != 0;
}

struct Stats { uint32_t reverted = 0u, stale = 0u, failed = 0u; };

// The dead-man pass: revert every live survivor, drop every stale record.
static Stats run_dead_man(RevertJournal& j, bool dry_run) {
    Stats s;
    const auto recs = j.recover();
    std::printf("  survivors in journal: %zu\n", recs.size());
    for (const auto& r : recs) {
        const bool live = RevertJournal::process_still_matches(r);
        std::printf("    pid=%-6u exe=%-20s status=%-8s prev=0x%llx  %s\n",
                    r.pid, r.exe_name[0] ? r.exe_name : "(none)",
                    to_string(r.status),
                    static_cast<unsigned long long>(r.prev_mask),
                    live ? "LIVE -> revert" : "STALE -> drop");
        if (dry_run) { if (live) ++s.reverted; else ++s.stale; continue; }

        if (live) {
            if (revert_to_prev(r.pid, r.prev_mask)) ++s.reverted;
            else                                    ++s.failed;
            j.mark_reverted(RevertKey{r.pid, r.creation_time});
        } else {
            // Process gone / pid recycled: the placement died with it. Mark the
            // record resolved so we never revisit it (idempotency).
            j.mark_reverted(RevertKey{r.pid, r.creation_time});
            ++s.stale;
        }
    }
    return s;
}

// ── --self-test — end-to-end dead-man proof (spawns a real child) ─────────────
static int self_test() {
    std::printf("\n=== phynned-revert --self-test (end-to-end dead-man) ===\n");
    int fail = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++fail;
    };

    char tmp[MAX_PATH]{};
    GetTempPathA(sizeof(tmp), tmp);
    const std::string jpath = std::string(tmp) + "phynned_revert_selftest.bin";
    DeleteFileA(jpath.c_str());

    // Spawn a benign, suspended cmd.exe as the placement target.
    char sysdir[MAX_PATH]{};
    GetSystemDirectoryA(sysdir, sizeof(sysdir));
    char cmdline[MAX_PATH + 16];
    std::snprintf(cmdline, sizeof(cmdline), "\"%s\\cmd.exe\"", sysdir);
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmdline, nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi)) {
        std::printf("  [FAIL] could not spawn cmd.exe (err=%lu)\n", GetLastError());
        return 1;
    }
    const uint32_t child = pi.dwProcessId;

    // Capture the child's real prev mask + creation-time, then NARROW its affinity
    // to core 0 so a successful revert is observably different.
    HANDLE hq = OpenProcess(kRevertAccess, FALSE, child);
    DWORD_PTR proc_mask = 0u, sys_mask = 0u;
    check(hq && GetProcessAffinityMask(hq, &proc_mask, &sys_mask),
          "read child's prev affinity mask");
    const uint64_t prev_mask = static_cast<uint64_t>(proc_mask);
    const uint64_t narrow    = 1ull; // core 0 only
    check(hq && SetProcessAffinityMask(hq, static_cast<DWORD_PTR>(narrow)),
          "narrow child's affinity to core 0 (the 'placement')");
    if (hq) CloseHandle(hq);
    const uint64_t child_ct = RevertJournal::query_creation_time(child);
    check(child_ct != 0u, "read child's creation-time");

    // Journal it write-ahead, then mark APPLIED (mirrors the live agent path).
    {
        RevertJournal j;
        check(j.open(jpath.c_str()), "open self-test journal");
        j.record_pending(child, "cmd.exe", child_ct, prev_mask, narrow);
        j.mark_applied(RevertKey{child, child_ct});
    }

    // ── Pass 1: the dead-man restores the child to prev_mask ──────────────────
    {
        RevertJournal j;
        check(j.open(jpath.c_str()), "reopen journal (fresh dead-man instance)");
        std::printf("  --- dead-man pass 1 ---\n");
        const Stats s = run_dead_man(j, /*dry_run=*/false);
        check(s.reverted == 1u && s.stale == 0u && s.failed == 0u,
              "pass 1 reverts exactly 1 live placement");
    }
    // Verify the child's affinity is actually back to prev_mask.
    HANDLE hv = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child);
    DWORD_PTR after = 0u, s2 = 0u;
    check(hv && GetProcessAffinityMask(hv, &after, &s2), "re-read child affinity");
    if (hv) CloseHandle(hv);
    check(static_cast<uint64_t>(after) == prev_mask,
          "child affinity restored to captured prev_mask");

    // ── Pass 2: idempotent — nothing left to do ───────────────────────────────
    {
        RevertJournal j;
        check(j.open(jpath.c_str()), "reopen journal for pass 2");
        std::printf("  --- dead-man pass 2 (idempotency) ---\n");
        const Stats s = run_dead_man(j, /*dry_run=*/false);
        check(s.reverted == 0u && s.stale == 0u && s.failed == 0u,
              "pass 2 is a no-op (idempotent)");
    }

    TerminateProcess(pi.hProcess, 0u);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DeleteFileA(jpath.c_str());

    std::printf("\n  SELF-TEST: %s (%d failure%s)\n",
                fail == 0 ? "PASS" : "FAIL", fail, fail == 1 ? "" : "s");
    return fail == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string journal;
    bool dry_run = false, do_self_test = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            journal = argv[++i];
        } else if (std::strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (std::strcmp(argv[i], "--self-test") == 0) {
            do_self_test = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: phynned-revert [--journal PATH] [--dry-run]"
                        " | --self-test\n");
            return 0;
        }
    }

    if (do_self_test) return self_test();

    if (journal.empty()) journal = RevertJournal::default_path();
    std::printf("phynned-revert — DR1 dead-man%s\n", dry_run ? " (dry-run)" : "");
    std::printf("  journal: %s\n", journal.c_str());

    RevertJournal j;
    if (!j.open(journal.c_str())) {
        std::printf("  no journal to process (nothing to revert)\n");
        return 0; // absence of a journal is success — nothing was orphaned
    }
    const Stats s = run_dead_man(j, dry_run);
    std::printf("  => reverted=%u  stale-dropped=%u  failed=%u%s\n",
                s.reverted, s.stale, s.failed, dry_run ? "  (dry-run)" : "");
    return (s.failed == 0u) ? 0 : 1;
}
// Made with my soul - Swately <3
