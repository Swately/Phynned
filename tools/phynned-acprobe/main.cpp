// tools/phynned-acprobe/main.cpp
// phynned-acprobe — standalone M0-safety proof harness for the CR1 AcProbe.
//
// Runs the least-privilege anti-cheat probe against a real (or synthetic) target
// and shows the operator EVERY handle open — or the absence of one for a refused
// title. This is the vehicle that proves the CR1 mitigation is safe before any
// ActionExecutor integration.
//
// Usage:
//   phynned-acprobe --pid <N>          probe a live pid (exe auto-detected)
//   phynned-acprobe --name <exe>       find a live pid by exe name, then probe
//   phynned-acprobe --self-test        spawn a benign cmd.exe + probe it (expect
//                                      ALLOWED, 1 open) AND classify a synthetic
//                                      AC title (expect Refused, 0 opens)
//   options: --mem <path> --audit <path>
//
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <phynned/observer/AcDriverOracle.hpp>
#include <phynned/learn/PerGameMemory.hpp>
#include <phynned/action/AuditLog.hpp>
#include <phynned/action/AcProbe.hpp>

using phynned::observer::AcDriverOracle;
using phynned::observer::AcClass;
using phynned::learn::PerGameMemory;
using phynned::action::AuditLog;
using phynned::action::AuditRecord;
using phynned::action::AcProbe;
using phynned::action::ProbeResult;
using phynned::action::kProbeAccess;

// ── Toolhelp helpers ──────────────────────────────────────────────────────────
static DWORD find_pid_by_name(const char* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0u;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    DWORD found = 0u;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, exe) == 0) { found = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Bounded, always-null-terminating copy (avoids strncpy truncation warnings).
static void copy_str(char* dst, size_t dst_sz, const char* src) {
    if (dst_sz == 0u) return;
    size_t n = 0u;
    if (src) for (; n + 1u < dst_sz && src[n]; ++n) dst[n] = src[n];
    dst[n] = '\0';
}

static bool exe_name_by_pid(DWORD pid, char* out, size_t out_len) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    bool ok = false;
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                copy_str(out, out_len, pe.szExeFile);
                ok = true;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return ok;
}

// ── Audit dump — read the raw AuditRecord stream and count opens ──────────────
static uint32_t dump_audit(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("    (audit file '%s' not found)\n", path); return 0u; }
    AuditRecord rec{};
    uint32_t idx = 0u, opens = 0u;
    while (std::fread(&rec, sizeof(rec), 1u, f) == 1u) {
        char ev[9]{};
        std::memcpy(ev, rec.event_type, 8);
        ev[8] = '\0';
        const bool is_open = (std::strcmp(ev, "ACOPEN") == 0);
        if (is_open) ++opens;
        std::printf("    [%u] %-8s pid=%-6lu rights=0x%04lx prev=0x%llx new=0x%llx success=%u%s\n",
                    idx, ev,
                    static_cast<unsigned long>(rec.target_pid),
                    static_cast<unsigned long>(rec.rule_id),
                    static_cast<unsigned long long>(rec.prev_affinity_mask),
                    static_cast<unsigned long long>(rec.new_affinity_mask),
                    rec.success,
                    is_open ? "   <-- OpenProcess ATTEMPT" : "");
        ++idx;
    }
    std::fclose(f);
    if (idx == 0u) std::printf("    (no audit records — ZERO handle opens)\n");
    return opens;
}

// ── One probe run — reset audit, classify, probe, dump ────────────────────────
struct RunOut { ProbeResult r; uint32_t opens; };

static RunOut run_one(const char* label, AcDriverOracle& oracle,
                      PerGameMemory& mem, const char* exe_name, DWORD pid,
                      const char* audit_path) {
    std::printf("\n=== %s ===\n", label);
    std::printf("  target       : pid=%lu exe=\"%s\"\n",
                static_cast<unsigned long>(pid), exe_name ? exe_name : "(null)");

    // Fresh audit file so open counts are exact for this run.
    DeleteFileA(audit_path);

    const AcClass klass = oracle.classify_foreground_game(exe_name);
    std::printf("  oracle class : %s  (probe_allowed=%s)\n",
                AcDriverOracle::to_string(klass),
                AcDriverOracle::probe_allowed(klass) ? "true" : "false");

    AuditLog audit;
    if (!audit.open(audit_path, GetCurrentProcessId())) {
        std::printf("  [WARN] could not open audit log at %s\n", audit_path);
    }

    const ProbeResult r = AcProbe::probe_and_label(pid, exe_name, oracle, mem, &audit);
    audit.close();   // flush all rows to disk

    const bool probed = (r == ProbeResult::Allowed || r == ProbeResult::Blocked);
    std::printf("  decision     : %s (%s)\n",
                probed ? "PROBED" : "did NOT probe",
                phynned::action::to_string(r));
    std::printf("  audit dump   :\n");
    const uint32_t opens = dump_audit(audit_path);
    std::printf("  => opens=%u result=%s\n", opens, phynned::action::to_string(r));
    return RunOut{ r, opens };
}

static const char* pf(bool ok) { return ok ? "PASS" : "FAIL"; }

int main(int argc, char** argv) {
    const char* name      = nullptr;
    DWORD       pid       = 0u;
    bool        self_test = false;
    char        mem_path[512]   = "";
    char        audit_path[512] = "";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pid = static_cast<DWORD>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (std::strcmp(argv[i], "--self-test") == 0) {
            self_test = true;
        } else if (std::strcmp(argv[i], "--mem") == 0 && i + 1 < argc) {
            copy_str(mem_path, sizeof(mem_path), argv[++i]);
        } else if (std::strcmp(argv[i], "--audit") == 0 && i + 1 < argc) {
            copy_str(audit_path, sizeof(audit_path), argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: phynned-acprobe [--pid N | --name exe | --self-test]"
                        " [--mem path] [--audit path]\n");
            return 0;
        }
    }

    // Default temp paths (never touch the operator's real memory.toml/audit.bin).
    char tmp[512];
    if (GetTempPathA(sizeof(tmp), tmp) == 0) std::strcpy(tmp, ".\\");
    if (mem_path[0]   == '\0') std::snprintf(mem_path,   sizeof(mem_path),   "%sphynned_acprobe_mem.toml", tmp);
    if (audit_path[0] == '\0') std::snprintf(audit_path, sizeof(audit_path), "%sphynned_acprobe_audit.bin", tmp);

    std::printf("phynned-acprobe — CR1 least-privilege AC probe (M0-safety)\n");
    std::printf("  probe access : 0x%04x (PROCESS_SET_INFORMATION 0x0200"
                " | PROCESS_QUERY_LIMITED_INFORMATION 0x1000)\n", kProbeAccess);
    std::printf("  mem   : %s\n  audit : %s\n", mem_path, audit_path);

    AcDriverOracle oracle;
    PerGameMemory  mem;
    // NOTE: hardware_id left blank in this standalone harness (skips the topology
    // dependency); find/upsert stay self-consistent within the process.
    if (self_test) {
        // Start the self-test from a clean label store so A actually PROBES
        // (a stale ALLOWED label from a prior run would short-circuit it).
        char bak[600], tmp[600];
        std::snprintf(bak, sizeof(bak), "%s.bak", mem_path);
        std::snprintf(tmp, sizeof(tmp), "%s.tmp", mem_path);
        DeleteFileA(mem_path); DeleteFileA(bak); DeleteFileA(tmp);
    }
    (void)mem.load(mem_path);   // non-fatal; empty on first run

    int rc = 0;

    if (self_test) {
        // Spawn a benign, suspended cmd.exe as the real handle-open target.
        char sysdir[MAX_PATH]{};
        GetSystemDirectoryA(sysdir, sizeof(sysdir));
        char cmdline[MAX_PATH + 16];
        std::snprintf(cmdline, sizeof(cmdline), "\"%s\\cmd.exe\"", sysdir);
        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        DWORD benign = GetCurrentProcessId();   // fallback: our own pid
        bool spawned = false;
        if (CreateProcessA(nullptr, cmdline, nullptr, nullptr, FALSE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi)) {
            benign = pi.dwProcessId; spawned = true;
        } else {
            std::printf("[WARN] could not spawn cmd.exe (err=%lu); using self pid %lu\n",
                        GetLastError(), static_cast<unsigned long>(benign));
        }

        // ── A — allow-classified title (Allow_B) on a benign pid: exercises the
        //        REAL open(0x1200) + no-op affinity set + ALLOWED label + audit.
        //        (On this box a live EA AC refuses UNKNOWN titles, so a probe-
        //         allowed CLASS is required to reach the open path — see A2.)
        const RunOut a = run_one(
            "A: Allow_B title on a benign pid (expect ALLOWED, opens=1)",
            oracle, mem, "league of legends.exe", benign, audit_path);

        // ── A' — re-probe the SAME identity: must short-circuit, ZERO opens.
        const RunOut a2 = run_one(
            "A': re-probe same title (expect AlreadyLabeledAllowed, opens=0)",
            oracle, mem, "league of legends.exe", benign, audit_path);

        // ── B — synthetic (c)/Unknown AC title, no live process: must refuse.
        const RunOut b = run_one(
            "B: synthetic AC title \"bf6.exe\" (expect Refused, opens=0)",
            oracle, mem, "bf6.exe", 0u, audit_path);

        // ── D — allow-classified title on a PROTECTED pid (System=4): the open
        //        attempt is audited, OpenProcess is denied -> BLOCKED label.
        const RunOut d = run_one(
            "D: Allow_B title on protected pid 4 (expect Blocked, opens=1)",
            oracle, mem, "valorant.exe", 4u, audit_path);

        // ── D' — re-probe the BLOCKED identity: must short-circuit, ZERO opens.
        const RunOut d2 = run_one(
            "D': re-probe blocked title (expect AlreadyLabeledBlocked, opens=0)",
            oracle, mem, "valorant.exe", 4u, audit_path);

        // ── C — informational: an UNKNOWN benign title under the live EA AC ──
        const RunOut c = run_one(
            "C (info): unknown \"cmd.exe\" under live EA AC (fail-safe Refused, opens=0)",
            oracle, mem, "cmd.exe", benign, audit_path);

        if (spawned) {
            TerminateProcess(pi.hProcess, 0u);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }

        std::printf("\n=== GATE SUMMARY ===\n");
        const bool a_ok  = (a.r  == ProbeResult::Allowed)               && a.opens  == 1u;
        const bool a2_ok = (a2.r == ProbeResult::AlreadyLabeledAllowed) && a2.opens == 0u;
        const bool b_ok  = (b.r  == ProbeResult::Refused_DoNotProbe)    && b.opens  == 0u;
        const bool d_ok  = (d.r  == ProbeResult::Blocked)               && d.opens  == 1u;
        const bool d2_ok = (d2.r == ProbeResult::AlreadyLabeledBlocked) && d2.opens == 0u;
        std::printf("  A  ALLOWED  benign pid, opens=%u        : %s (%s)\n",
                    a.opens, pf(a_ok), phynned::action::to_string(a.r));
        std::printf("  A' no re-probe (ALLOWED), opens=%u       : %s (%s)\n",
                    a2.opens, pf(a2_ok), phynned::action::to_string(a2.r));
        std::printf("  B  REFUSED  synthetic AC, opens=%u       : %s (%s)\n",
                    b.opens, pf(b_ok), phynned::action::to_string(b.r));
        std::printf("  D  BLOCKED  denied open, opens=%u        : %s (%s)\n",
                    d.opens, pf(d_ok), phynned::action::to_string(d.r));
        std::printf("  D' no re-probe (BLOCKED), opens=%u       : %s (%s)\n",
                    d2.opens, pf(d2_ok), phynned::action::to_string(d2.r));
        std::printf("  C  info: live-AC fail-safe on cmd.exe    : %s (opens=%u)\n",
                    phynned::action::to_string(c.r), c.opens);
        rc = (a_ok && a2_ok && b_ok && d_ok && d2_ok) ? 0 : 1;
        std::printf("  GATE: %s\n", rc == 0 ? "PASS" : "FAIL");
    } else {
        char detected[64] = "";
        if (pid != 0u && (name == nullptr)) {
            if (exe_name_by_pid(pid, detected, sizeof(detected))) name = detected;
        } else if (name != nullptr && pid == 0u) {
            pid = find_pid_by_name(name);
            if (pid == 0u) {
                std::printf("[info] no live process named '%s'; classifying name only"
                            " (pid=0 — refused titles never touch the pid)\n", name);
            }
        }
        if (name == nullptr) {
            std::printf("nothing to do — pass --pid, --name, or --self-test"
                        " (see --help)\n");
            (void)mem.save(mem_path);
            return 2;
        }
        run_one("PROBE", oracle, mem, name, pid, audit_path);
    }

    (void)mem.save(mem_path);   // persist the permanent label(s)
    return rc;
}
// Made with my soul - Swately <3
