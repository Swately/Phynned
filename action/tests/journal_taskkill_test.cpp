// action/tests/journal_taskkill_test.cpp
// DR1 END-TO-END: revert-to-captured-prev survives a taskkill of the agent.
//
// This is the live CR1/DR1 proof for the S3 integration: the write-ahead
// RevertJournal wired into ActionExecutor must restore a placed process to its
// CAPTURED prior affinity after the setter dies WITHOUT running revert_all().
//
// The scenario, on the box:
//   1. Spawn a child process (this same exe, `child` arg → just sleeps) as the
//      placement target.
//   2. Give the child a NON-default pre-existing affinity (0x3 = cores 0,1).
//      This is the "user's own affinity" — distinct from BOTH the mask Phynned
//      applies (0x1) AND the all-cores default, so restoring it proves
//      revert-to-CAPTURED-prev, not the old revert-to-default.
//   3. An ActionExecutor applies a PinAffinity placement narrowing to 0x1. This
//      captures prev=0x3, write-ahead-journals it, then Sets → child at 0x1.
//   4. TASKKILL SIMULATION: leak the ActionExecutor (operator new, never
//      deleted) so ~ActionExecutor → revert_all() NEVER runs. The placement is
//      orphaned (child stays 0x1); the journal APPLIED record persists on disk.
//   5. A FRESH ActionExecutor runs recover() in its constructor (BEFORE any tick
//      loop). It sees the surviving APPLIED record, confirms the child still
//      matches (pid + creation-time + exe), and restores it to the captured
//      prev 0x3, then marks the record REVERTED.
//   6. Assert the child affinity == 0x3 (captured prev), NOT 0x1, NOT all-cores.
//
// Isolation: LOCALAPPDATA is redirected to a fresh temp dir so RevertJournal's
// default_path() lands in the test's own journal — the operator's real journal
// is never touched.
//
// Privilege: none — setting affinity on one's OWN child needs no admin.
//
#include <phynned/action/ActionExecutor.hpp>
#include <phynned/action/RevertJournal.hpp>
#include <phynned/policy/PolicyDecision.hpp>
#include <phyriad/topology/HardwareTopology.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

using namespace phynned::action;
using namespace phynned::policy;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static uint64_t read_affinity(uint32_t pid) {
    const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return 0ull;
    DWORD_PTR pm = 0, sm = 0;
    const BOOL ok = GetProcessAffinityMask(h, &pm, &sm);
    CloseHandle(h);
    return ok ? static_cast<uint64_t>(pm) : 0ull;
}

int main(int argc, char** argv) {
    // ── Child mode: a stable live target. Just sleep; the parent kills us. ────
    if (argc >= 2 && std::string(argv[1]) == "child") {
        Sleep(120000);
        return 0;
    }

    std::printf("journal_taskkill_test — DR1 revert-to-captured-prev after taskkill\n");

    // ── Isolate the journal (redirect LOCALAPPDATA to a fresh temp dir) ───────
    char tmp[MAX_PATH]{};
    GetTempPathA(sizeof(tmp), tmp);
    const std::string appdir = std::string(tmp) + "phynned_taskkill_test";
    CreateDirectoryA(appdir.c_str(), nullptr);
    SetEnvironmentVariableA("LOCALAPPDATA", appdir.c_str());
    // default_path() → <appdir>\Phynned\revert_journal\journal.bin — wipe it so
    // Phase-1's recover() starts from an empty journal.
    const std::string jpath = appdir + "\\Phynned\\revert_journal\\journal.bin";
    DeleteFileA(jpath.c_str());

    // ── Spawn the child target (this exe with `child`) ────────────────────────
    char selfexe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, selfexe, sizeof(selfexe));
    std::string base = selfexe;
    if (const std::size_t s = base.find_last_of("\\/"); s != std::string::npos)
        base = base.substr(s + 1);   // basename, used as the plumbed exe_name

    std::string cmd = std::string("\"") + selfexe + "\" child";
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL spawned = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr,
                                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &si, &pi);
    if (!spawned) {
        std::printf("  [FAIL] CreateProcess(child) failed (err=%lu)\n", GetLastError());
        return 1;
    }
    const uint32_t child = static_cast<uint32_t>(pi.dwProcessId);
    Sleep(300);   // let the child settle so affinity ops land

    const uint32_t ncpu = phyriad::hw::topology().logical_core_count();
    check(ncpu >= 2u, "system has >= 2 logical cores (test precondition)");

    const uint64_t kPrev = 0x3ull;   // cores 0,1  — the pre-existing "user" mask
    const uint64_t kNew  = 0x1ull;   // core 0     — Phynned's placement

    // ── Set the NON-default pre-existing affinity directly (not via executor) ─
    if (const HANDLE hset =
            OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, child)) {
        SetProcessAffinityMask(hset, static_cast<DWORD_PTR>(kPrev));
        CloseHandle(hset);
    }
    check(read_affinity(child) == kPrev,
          "child pre-existing affinity == 0x3 (the captured-prev target)");

    // ── Phase 1: apply a placement, then "taskkill" (leak → no revert_all) ────
    {
        ActionExecutor* exec = new ActionExecutor();   // journal opens clean
        PolicyDecision d{};
        d.target_pid  = child;
        d.rule_id     = 1u;
        d.action_kind = ActionKind::PinAffinity;
        d.core_mask   = kNew;
        // Plumb the exe basename; creation_time self-queried (== AgentRuntime path).
        const auto r = exec->apply(d, base.c_str(), /*creation_time=*/0ull);
        check(r.has_value(), "apply(PinAffinity 0x1) succeeded (journaled APPLIED)");
        check(read_affinity(child) == kNew, "child affinity narrowed to 0x1 by apply");
        // *** TASKKILL: leak exec — ~ActionExecutor/revert_all() never runs. ***
        (void)exec;
    }
    check(read_affinity(child) == kNew,
          "placement ORPHANED after taskkill (still 0x1; no RAII revert ran)");

    // ── Phase 2: a fresh ActionExecutor recovers on construction ──────────────
    {
        ActionExecutor fresh;   // constructor recover() runs BEFORE any tick loop
        const uint64_t after = read_affinity(child);
        check(after == kPrev,
              "recover() restored child to CAPTURED prev 0x3 "
              "(NOT default all-cores, NOT the applied 0x1)");
        if (after != kPrev)
            std::printf("      observed affinity after recover = 0x%llx\n",
                        static_cast<unsigned long long>(after));
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::printf("%s (%d failures)\n",
                g_fail == 0 ? "journal_taskkill_test PASSED"
                            : "journal_taskkill_test FAILED",
                g_fail);
    return g_fail == 0 ? 0 : 1;
}
// Made with my soul - Swately <3
