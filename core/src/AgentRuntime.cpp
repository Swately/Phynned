// core/src/AgentRuntime.cpp
// AgentRuntime — implementation.
//

#include <phynned/core/AgentRuntime.hpp>
#include <phynned/core/AdaptiveTick.hpp>
#include <phynned/core/Diag.hpp>
#include <phynned/core/SelfMonitor.hpp>
#include <phynned/core/PowerWatch.hpp>
#include <phynned/core/AutoRevertGuard.hpp>
#include <phynned/core/InternalWatchdog.hpp>
#include <phynned/core/IdleWatch.hpp>
#include <phynned/core/UserPinRule.hpp>   // W3 — pure user-pin decision (R1/R4)
#include <phynned/core/UserRuleLookup.hpp> // W3 — rule lookup w/ lazy path resolve
#include <phynned/observer/ProcessObserver.hpp>
#include <phynned/observer/MetricsCollector.hpp>
#include <phynned/observer/ProcessClassifier.hpp>
#include <phynned/observer/KindOverrides.hpp>
#include <phynned/observer/ForegroundWatcher.hpp>
#include <phynned/observer/ClassificationCache.hpp>
#include <phynned/observer/EtwProviderSet.hpp>
#include <phynned/policy/PolicyEngine.hpp>
#include <phynned/policy/AutoPolicySelector.hpp>
#include <phynned/action/ActionExecutor.hpp>
#include <phynned/action/AcProbe.hpp>          // CR1 — anti-cheat gate before any placement on a game
#include <phynned/action/RevertJournal.hpp>    // DR1 — query_creation_time for the apply plumbing
#include <phynned/observer/AcDriverOracle.hpp> // CR1 — zero-handle AC class oracle
#include <phynned/action/AuditLog.hpp>
#include <phynned/bench/ABRunner.hpp>
#include <phynned/config/ConfigStore.hpp>
#include <phynned/config/DefaultPolicyPack.hpp>
#include <phynned/learn/PerGameMemory.hpp>
#include <phynned/learn/RouteAdvisor.hpp>       // MR-1 — SHADOW ROUTER (advice only; never places)
#include <phynned/ipc/PhynnedAgentPublisher.hpp>

#include <phyriad/topology/HardwareTopology.hpp>
#include <phyriad/tuning/PrivilegeCheck.hpp>
#include <phyriad/tuning/WorkingSet.hpp>   // FR-19 — set_self_working_set
#include <phyriad/hal/Timestamp.hpp>
#include <phyriad/hal/WakeEvent.hpp>       // FR-16 — cross-platform wake event
#include <phyriad/process/CurrentProcess.hpp> // FR-18 — self_pid / self_name
#include <phyriad/process/ProcessEnumerator.hpp> // E5 hardened coexistence full-scan

#include <algorithm>
#include <array>
#include <bitset>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <phyriad/hal/MemoryOrder.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
// psapi.h no longer needed here — moved to MetricsCollector_win32.cpp
// getpid() replaced by phyriad::proc::self_pid() (FR-18)
#endif

namespace phynned::core {

// ── Diag storage (declared in Diag.hpp) ───────────────────────────────────
// Updated by run() at each phase transition; read by main.cpp's unhandled
// exception filter to surface the last reached phase on crash.
namespace diag {
    std::atomic<uint32_t> g_last_phase{0u};
    std::atomic<uint32_t> g_last_tick{0u};
    std::atomic<uint32_t> g_last_apply_pid{0u};
    std::atomic<uint32_t> g_last_apply_rule{0u};
    std::atomic<uint32_t> g_last_apply_tid{0u};
}

// Local helper macro — sets g_last_phase with relaxed store.
#define PHYNNED_PHASE(P) \
    ::phynned::core::diag::g_last_phase.store(static_cast<uint32_t>(P), \
                                            std::memory_order_relaxed)  // HAL: relaxed — secondary atomic in compound op

// ── Auto-discover blacklist ────────────────────────────────────
// Hard-coded list of Windows shell / OS UI processes that pass the
// fullscreen + D3D heuristic (because DWM composites them with D3D and
// they can occupy the screen when the desktop is foreground) but are
// NEVER games. Without this guard, alt-tabbing to the desktop registers
// explorer.exe as a "discovered game", inflating observer patterns with
// garbage.
//
// Bug seen in agent_hot_thread_v2.log:
//   [Phynned] Auto-discovered foreground game: explorer.exe (PID 2692) — pattern registered
//
// Case-insensitive exact-match on the basename. Keep the list to clear
// OS shell processes only; debatable cases (Taskmgr, WindowsTerminal,
// conhost, third-party launchers like steamwebhelper) are left out so
// users can still pin them if they want — the classifier's other gates
// (kind != Game) will prevent inappropriate policy application.
static bool is_os_shell_process(const char* exe_name) noexcept {
    if (!exe_name || exe_name[0] == '\0') return false;
    static constexpr const char* kShellExes[] = {
        "explorer.exe",                  // Windows desktop / file manager
        "dwm.exe",                       // Desktop Window Manager
        "ApplicationFrameHost.exe",      // UWP/XAML window host
        "SearchHost.exe",                // Win11 search
        "ShellExperienceHost.exe",       // Action Center, etc.
        "StartMenuExperienceHost.exe",   // Start menu (Win11)
        "TextInputHost.exe",             // Touch keyboard, IME UI
        "LockApp.exe",                   // Lock screen
        "SystemSettings.exe",            // Settings (UWP)
        "LogonUI.exe",                   // Login screen
        "Widgets.exe",                   // Win11 widgets panel
        "WidgetService.exe",             // Win11 widgets backend
        "ctfmon.exe",                    // Text Services Framework
    };
    for (const char* shell : kShellExes) {
#ifdef _WIN32
        if (_stricmp(exe_name, shell) == 0) return true;
#else
        // Linux paths use case-sensitive compare since these shell names
        // are Windows-specific anyway.
        if (std::strcmp(exe_name, shell) == 0) return true;
#endif
    }
    return false;
}

// ── Get short exe name from PID ───────────────────────
// Returns true on success and writes the basename (no path) into `out`.
// Used by foreground-heuristic auto-discovery to register dynamic patterns.
//
// Note: uses PROCESS_QUERY_LIMITED_INFORMATION (less invasive than
// PROCESS_VM_READ — granted to non-admin callers for most processes). Anti-
// cheat-protected games typically still allow QueryFullProcessImageNameW
// even when they deny PROCESS_VM_READ.
static bool get_process_short_name(uint32_t pid,
                                    char* out, uint32_t out_cap) noexcept {
#ifdef _WIN32
    if (out_cap == 0u) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
    if (!h) return false;
    wchar_t wpath[MAX_PATH]{};
    DWORD sz = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(h, 0u, wpath, &sz);
    CloseHandle(h);
    if (!ok || sz == 0u) return false;
    // Find the basename — last '\' or '/'
    const wchar_t* base = wpath;
    for (DWORD i = 0u; i < sz; ++i) {
        if (wpath[i] == L'\\' || wpath[i] == L'/') base = wpath + i + 1u;
    }
    // Narrow-encode ASCII (non-ASCII → '?'). Sufficient for our pattern match.
    uint32_t out_i = 0u;
    while (*base != L'\0' && out_i < out_cap - 1u) {
        out[out_i++] = (*base < 0x80) ? static_cast<char>(*base) : '?';
        ++base;
    }
    out[out_i] = '\0';
    return out_i > 0u;
#else
    (void)pid; (void)out; (void)out_cap;
    return false;
#endif
}

// ── Self-pinning: pick the "least valuable" core ──────────────────────────
static void apply_self_pin(const phyriad::HardwareTopology& topo) noexcept {
#ifdef _WIN32
    // Intel hybrid: prefer E-cores (efficiency_class == 0 on Intel hybrid)
    for (const auto& c : topo.cores) {
        if (c.is_efficiency_core) {
            const DWORD_PTR mask = static_cast<DWORD_PTR>(1ull) << c.logical_id;
            SetThreadAffinityMask(GetCurrentThread(), mask);
            std::fprintf(stdout,
                "[Phynned] Self-pinned to E-core %u (logical)\n", c.logical_id);
            return;
        }
    }

    // AMD X3D: prefer first core that does NOT have V-Cache
    bool has_vcache_cpu = false;
    for (const auto& c : topo.cores) {
        if (c.has_v_cache) { has_vcache_cpu = true; break; }
    }
    if (has_vcache_cpu) {
        for (const auto& c : topo.cores) {
            if (!c.has_v_cache) {
                const DWORD_PTR mask = static_cast<DWORD_PTR>(1ull) << c.logical_id;
                SetThreadAffinityMask(GetCurrentThread(), mask);
                std::fprintf(stdout,
                    "[Phynned] Self-pinned to non-V-Cache core %u\n", c.logical_id);
                return;
            }
        }
    }

    // Fallback: last logical core
    if (!topo.cores.empty()) {
        const uint32_t last = topo.cores.back().logical_id;
        const DWORD_PTR mask = static_cast<DWORD_PTR>(1ull) << last;
        SetThreadAffinityMask(GetCurrentThread(), mask);
        std::fprintf(stdout,
            "[Phynned] Self-pinned to last core %u (fallback)\n", last);
    }
#else
    (void)topo;
#endif
}

// ── Memory budget enforcement (§3.3) ────────────────────────────────────
// phyriad::tuning::set_self_working_set (FR-19) — cross-platform working-set hint.
// Returns an error on restricted environments (e.g. low-privilege CI) — log
// but never abort, the agent degrades gracefully without the hint.
static void apply_memory_budget() noexcept {
    constexpr uint64_t kMinBytes = 16ull * 1024u * 1024u;  // 16 MB
    constexpr uint64_t kMaxBytes = 50ull * 1024u * 1024u;  // 50 MB
    const auto r = phyriad::tuning::set_self_working_set(kMinBytes, kMaxBytes);
    if (!r.has_value()) {
        // Non-fatal: log and continue.  Likely PermissionDenied on restricted CI.
        std::fprintf(stdout,
            "[Phynned] Working-set hint skipped (code=%u) — continuing.\n",
            static_cast<unsigned>(r.error().code));
    }
}

// ══ MR-2 BACKGROUND CORRAL (DRY-RUN default) ══════════════════════════════
// The corral moves ACTIVE, non-game background off the V-Cache CCD onto the
// Frequency CCD (mask 0xFFFF0000 on the 7950X3D), keeping the 96 MB V-Cache
// clean for games / measured cache-winners. It defaults to DRY-RUN: it computes
// what it WOULD place and surfaces it, but calls ZERO Set* on any real process
// unless the explicit LIVE switch is on (kPhynnedCmdSetCorralLive / --corral-live).

/// Rule-id stamped on corral placements in the audit log (M4a). Distinct from the
/// policy-engine rule ids (1..8), the differential pin (7), and the memory
/// sentinel (99) so a corral row is unambiguous in audit.bin.
static constexpr uint32_t kCorralRuleId = 20u;

/// Rule-id stamped on W3 user "Always" pins in the audit log. Distinct from the
/// policy-engine rule ids (1..8), the differential pin (7), the memory sentinel
/// (99) and the corral (20) so a user-pin row is unambiguous, and so the R3
/// selective revert can target user pins independently of corral placements.
static constexpr uint32_t kUserRuleId = 21u;

/// The EFFECTIVE profile for placement logic: Full is reserved for MR-3 (the A/B
/// engine) and, until it lands, behaves as GamesCorral (W4 fallback).
static config::Profile effective_profile(config::Profile p) noexcept {
    return (p == config::Profile::Full) ? config::Profile::GamesCorral : p;
}

/// Active-CPU floor for a corral candidate. Matches the shadow router's herd
/// floor (learn::RouteAdvisor::kActiveCpuPct = 3%): below it the process is idle
/// herd and left alone (M3 "lo más barato"; do-no-harm).
static constexpr float kCorralActiveCpuPct = 3.0f;

// ── Fix B: the CORRAL action-layer denylist (E6, allowlist-primary) ────────
// EXPLICIT exclusion set for the corral, SEPARATE from detection's touchable
// filter (ProcessObserver::is_touchable / kSystemNames): even if one of these is
// somehow observed, the corral never re-pins it. Second line of defence behind
// the allowlist framing (kind != Game/System + not placement-eligible). Names are
// matched case-insensitively (exact basename).
static bool is_corral_denied(const char* exe_name) noexcept {
    if (!exe_name || exe_name[0] == '\0') return true;  // no name → never touch
    static constexpr const char* kCorralDeny[] = {
        // Real-time audio (audiodg-class is the documented top folk-failure).
        "audiodg.exe",
        // Shell / text-input / console host.
        "ctfmon.exe",   "conhost.exe",   "dwm.exe",
        // Core OS / session processes.
        "wininit.exe",  "csrss.exe",     "services.exe",
        "smss.exe",     "winlogon.exe",  "lsass.exe",
        "fontdrvhost.exe",
        // VM worker / WSL memory host.
        "vmmem.exe",    "vmmemwsl.exe",  "vmcompute.exe",
        // AMD 3D V-Cache Optimizer service (E5 owner of the game-placement policy).
        "amd3dvcachesvc.exe", "amd3dvcachedetection.exe",
        // EDR / anti-cheat SERVICE processes (the protected GAME is handled
        // separately by the AcDriverOracle zero-handle title map).
        "easyanticheat.exe", "easyanticheatservice.exe", "easyanticheat_eos.exe",
        "beservice.exe", "beserviceglobal.exe",
        "vgc.exe",       "vgtray.exe",
        "msmpeng.exe",   "securityhealthservice.exe",
    };
    for (const char* d : kCorralDeny) {
#ifdef _WIN32
        if (_stricmp(exe_name, d) == 0) return true;
#else
        if (std::strcmp(exe_name, d) == 0) return true;
#endif
    }
    return false;
}

// ── E5 coexistence: a CONFLICTING CPU-affinity optimizer running? ──────────
// E5 says detect the AMD 3D V-Cache service, Process Lasso, and Game Mode and
// "defer/bound" to avoid flapping. Crucially those split into two classes:
//
//   COMPLEMENTARY (do NOT block) — the AMD 3D V-Cache Optimizer service. It
//   routes GAMES between the CCDs; the corral EXCLUDES games (cedes them to it)
//   and only touches non-game background, so their target sets are DISJOINT —
//   no flapping. Blocking on it would be wrong: it runs permanently on every
//   7950X3D box, which would make the live corral impossible forever. We defer
//   to it exactly as E5 wants — by never touching a game — not by standing down.
//   (Belt-and-suspenders: the per-process self-managed-mask exclusion in
//   evaluate_corral() means any process another tool has pinned is skipped.)
//
//   CONFLICTING (force DRY-RUN) — broad affinity managers that pin ARBITRARY /
//   background processes, which is exactly the corral's target set: Process
//   Lasso (ProcessLasso.exe / ProcessGovernor.exe). Two of these fighting over
//   the same background process is the documented flapping failure.
//
// Case-insensitive substring match on the exe basename.
static bool is_coexistence_optimizer(const char* exe_name) noexcept {
    if (!exe_name || exe_name[0] == '\0') return false;
    static constexpr const char* kCoexist[] = {
        "processlasso",     // Process Lasso UI
        "processgovernor",  // Process Lasso background service
        // NOTE: "amd3dvcache" deliberately NOT here — complementary, see above.
    };
    // Lower-case the basename once, then substring-search.
    char lower[64]{};
    uint32_t n = 0u;
    for (; exe_name[n] != '\0' && n < sizeof(lower) - 1u; ++n) {
        const char c = exe_name[n];
        lower[n] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';
    for (const char* needle : kCoexist) {
        if (std::strstr(lower, needle) != nullptr) return true;
    }
    return false;
}

// ── The corral decision (PURE — cannot place) ──────────────────────────────
// Mirrors the RouteAdvisor's structural safety: this returns a plain value with
// NO reference to policy_engine / decision_buf / executor. A corral placement can
// ONLY happen where run() feeds this decision to executor.apply, and that call is
// gated behind the LIVE switch (see §3c). Decoupling the decision from the action
// is what makes the DRY-RUN default provably no-op.
struct CorralDecision {
    bool        would_corral{false};  ///< all predicates hold → candidate
    uint64_t    target_mask{0ull};    ///< Frequency-CCD mask to apply (0 if not)
    const char* reason{""};           ///< M4a human-readable rule hit (static str)
};

/// Evaluate the corral predicate for one candidate. Corral IFF ALL hold:
///   - kind != Game  AND  not placement-eligible   (games untouched — AMD's job)
///   - kind != System  AND  not in the Fix-B corral denylist
///   - the box is an X3D dual-CCD (vcache_mask != 0, freq_mask != 0; else E2 no-op)
///   - RouteAdvisor advice != VCache                (do-no-harm: never pull a
///     cache-candidate off V-Cache)
///   - active: cpu_usage_pct >= kCorralActiveCpuPct
///   - current affinity is readable (!= 0; else PPL/denied), touches a V-Cache
///     core (else already off it → no-op), and is the FULL unrestricted system
///     mask (else self-managed / already-restricted → respect it, E6).
static CorralDecision evaluate_corral(
    const observer::TargetProcess& tp,
    const observer::TargetMetrics& tm,
    bool                           is_placement_eligible,
    uint32_t                       vcache_mask,
    uint32_t                       system_mask,
    uint64_t                       freq_mask) noexcept
{
    CorralDecision cd{};
    // Games / the existing AC-gated placement path — never corralled.
    if (tp.kind == observer::TargetKind::Game) return cd;
    if (is_placement_eligible)                 return cd;
    // System + the explicit Fix-B action-layer denylist.
    if (tp.kind == observer::TargetKind::System) return cd;
    if (is_corral_denied(tp.name))               return cd;
    // Graceful degradation on non-X3D / homogeneous boxes (E2).
    if (vcache_mask == 0u || freq_mask == 0ull) return cd;
    // Do-no-harm: a cache-advised (VCache) candidate is never pulled off V-Cache.
    if (tm.advice_ccd == static_cast<uint8_t>(learn::RouteAdvice::Ccd::VCache))
        return cd;
    // Active only — the idle herd is left alone (M3).
    if (tm.cpu_usage_pct < kCorralActiveCpuPct) return cd;
    // Current-placement gates (need current_core_mask from Fix A).
    const uint32_t cur = tm.current_core_mask;
    if (cur == 0u)                     return cd;  // PPL / open denied → don't touch
    if ((cur & vcache_mask) == 0u)     return cd;  // already off V-Cache → no-op
    if (cur != system_mask)            return cd;  // self-managed / restricted (E6)
    // All predicates hold → would corral to the Frequency CCD.
    cd.would_corral = true;
    cd.target_mask  = freq_mask;
    cd.reason = "corral: active bg on V-Cache CCD, not cache-advised -> Freq CCD";
    return cd;
}

// ── Pimpl ─────────────────────────────────────────────────────────────────
struct AgentRuntime::Impl {
    AgentConfig                     cfg;
    std::atomic<bool>               stop_requested{false};
    std::atomic<bool>               running{false};
    std::atomic<uint32_t>           tick_count{0u};
    std::atomic<uint32_t>           active_targets_n{0u};
    WorkloadState                   workload_state{WorkloadState::Idle};
    bool                            is_admin{false};
    // Full PrivilegeLevel as returned by check_privilege_level — published
    // verbatim to SHM so the UI can distinguish None / Partial / Elevated /
    // Admin instead of inferring from is_admin + etw_active (which produced
    // a misleading 0/1/2 encoding that the UI read as "no privileges" even
    // when the process was UAC-elevated).
    phyriad::tuning::PrivilegeLevel privilege_level{
        phyriad::tuning::PrivilegeLevel::None};
    bool                            etw_active{false};

    // ── Core subsystems ───────────────────────────────────────────────────
    observer::ProcessObserver       observer;
    observer::MetricsCollector      metrics;
    policy::PolicyEngine            policy_engine;
    action::ActionExecutor          executor;
    // CR1 — zero-handle anti-cheat class oracle. Owned here; fed the foreground
    // game's exe name by AcProbe before any placement on a game (see §6 apply
    // loop). The permanent per-exe ALLOWED/BLOCKED label lives in `per_game`.
    observer::AcDriverOracle        ac_oracle{};
    SelfMonitor                     self_monitor;
    PowerWatch                      power_watch;
    IdleWatch                       idle_watch;
    InternalWatchdog                watchdog;

    // ── Auto-mode pipeline ────────────────────────────────────────────────
    observer::ProcessClassifier     classifier;
    observer::ClassificationCache   class_cache;
    observer::ForegroundWatcher     fg_watcher;
    // User-editable manual overrides (UI writes the file; agent reloads it
    // on mtime change). Consulted by `classifier` before its built-in
    // heuristics — see ProcessClassifier::classify().
    observer::KindOverrides         overrides;
    AutoRevertGuard                 revert_guard;
    policy::AutoPolicySelector      auto_selector{}; // §9.2 decision table

    // ── ETW dynamic switching (§4.5) ──────────────────────────────────────
    observer::EtwSessionManager     etw_mgr{3u};   // 3-tick hysteresis

    // ── Persistence ───────────────────────────────────────────────────────
    config::AgentConfig             agent_cfg{};   // from policies.toml
    learn::PerGameMemory            per_game{};    // from memory.toml (~48 KB)

    // ── MR-1 SHADOW ROUTER (advice only; structurally cannot place) ───────
    // Caches the V-Cache core mask at construction. Consulted each tick AFTER
    // metrics/classification; its RouteAdvice output is written onto metrics_buf
    // (advice_ccd/advice_confidence) and logged — it NEVER feeds decision_buf,
    // the policy engine, or the executor. See §3b in run().
    learn::RouteAdvisor             route_advisor{};
    // Advice-change rate-limit cache (M3): remember the last logged advice per
    // pid so the [SHADOW] M4a log fires only on a CHANGE, not every tick. Bounded
    // linear-probe table; overflow simply skips logging (never grows, never spams).
    struct AdviceLogSlot { uint32_t pid{0u}; uint8_t last_ccd{0xFFu}; };
    static constexpr uint32_t kAdviceLogSlots      = 128u;
    static constexpr uint32_t kMaxAdviceLogPerTick = 16u;  // hard per-tick line cap
    std::array<AdviceLogSlot, kAdviceLogSlots> advice_log_cache{};

    // ── MR-2 BACKGROUND CORRAL (DRY-RUN default; structurally cannot place
    //    while corral_live == false — the executor.apply call in §3c is gated
    //    behind it). corral_live flips ONLY via the explicit UI/CLI switch. ──
    bool     corral_live{false};              ///< the LIVE switch (default OFF)
    bool     coexist_optimizer_present{false};///< E5: AMD V-Cache svc / Process Lasso
    bool     coexist_full_scan_result{false}; ///< E5 hardened: last full-process scan verdict
    uint32_t coexist_scan_counter{0u};        ///< ticks since last full coexistence scan
    uint32_t self_pid{0u};                    ///< agent's own PID — never corral self
    uint32_t vcache_core_mask{0u};            ///< V-Cache CCD cores (0x0000FFFF on X3D)
    uint32_t system_core_mask{0u};            ///< all logical cores (0xFFFFFFFF on X3D)
    uint64_t freq_corral_mask{0ull};          ///< Frequency CCD target (0xFFFF0000)
    // Corral dry-run/live log rate-limit: log only on a would-corral CHANGE per
    // pid (same bounded direct-mapped table pattern as advice_log_cache).
    struct CorralLogSlot { uint32_t pid{0u}; uint8_t last_state{0xFFu}; };
    static constexpr uint32_t kMaxCorralLogPerTick = 16u;
    std::array<CorralLogSlot, kAdviceLogSlots> corral_log_cache{};

    // ── W3 per-process user rules (persistent, from policies.toml) ─────────
    // agent_cfg holds the rules (single writer). user_rules_generation bumps on
    // every mutation (SetProcessRule / RemoveProcessRule) so a stale UI remove is
    // rejected. user_rule_flags carries the dynamic per-rule status bits
    // (blocked_by_ac / flap_warn) recomputed each tick by the user-pin pass;
    // shm_user_rules is the off-stack scratch built for publish_user_rules.
    uint32_t user_rules_generation{0u};
    uint8_t  user_rule_flags[config::AgentConfig::kMaxProcessRules]{};
    alignas(8) std::array<ipc::UserRuleShm,
        config::AgentConfig::kMaxProcessRules> shm_user_rules{};
    // Rate-limit the user-pin log to a state CHANGE per pid (same bounded
    // direct-mapped table pattern as corral_log_cache).
    struct UserPinLogSlot { uint32_t pid{0u}; uint8_t last_state{0xFFu}; };
    std::array<UserPinLogSlot, kAdviceLogSlots> user_pin_log_cache{};

    // ── A/B benchmark runner ──────────────────────────────────────────────
    bench::ABRunner                 ab_runner{};   // controlled via IPC commands

    // ── SHM publisher ─────────────────────────────────────────────────────
    ipc::PhynnedAgentPublisher        publisher{};   // write side of shared memory

    // ── Persistent audit trail (§9.1) ─────────────────────────────────────
    action::AuditLog                audit_log{};
    uint64_t                        audit_cursor{0ull};  // drain cursor for exec log

    // ── TSC frequency (cached at start) ───────────────────────────────────
    uint64_t tsc_freq{2'000'000'000ull};

    // ── Working buffers (pre-allocated, reused every tick) ────────────────
    alignas(64) std::array<observer::TargetProcess, observer::kMaxTargets> target_buf{};
    alignas(64) std::array<observer::TargetMetrics, observer::kMaxTargets> metrics_buf{};
    alignas(64) std::array<policy::PolicyDecision, policy::kMaxDecisionsPerCycle> decision_buf{};
    uint32_t n_targets  {0u};
    uint32_t n_decisions{0u};

    // ── MASS-router working buffers (Impl members → off the run() stack) ──
    // Detection is now track-all-touchable, so target_buf can hold hundreds. Two
    // consequences handled here:
    //   1. PLACEMENT must stay exactly pre-MASS: only the pattern-ELIGIBLE subset
    //      is fed to the policy engine, so observing the full herd can never make a
    //      non-eligible process reach executor.apply (the mass-routing policy is a
    //      separate later task). eligible_* holds that subset.
    //   2. At the MASS cap these arrays would be multi-KB stack temporaries per
    //      tick; they live in the (heap-allocated) Impl instead.
    alignas(64) std::array<observer::TargetProcess, observer::kMaxTargets> eligible_targets{};
    alignas(64) std::array<observer::TargetMetrics, observer::kMaxTargets> eligible_metrics{};
    uint32_t n_eligible{0u};
    alignas(64) std::array<observer::TargetProcess, observer::kMaxTargets> filtered_targets{};
    alignas(64) std::array<observer::TargetMetrics, observer::kMaxTargets> filtered_metrics{};
    // Bounded top-N view copied into the SHM — the UI contract stays kMaxShmTargets.
    alignas(64) std::array<observer::TargetProcess, observer::kMaxShmTargets> shm_targets{};
    alignas(64) std::array<observer::TargetMetrics, observer::kMaxShmTargets> shm_metrics{};

    // ── Wake event for early tick (FR-16) ────────────────────────────────
    // phyriad::hal::WakeEvent — cross-platform auto-reset event.
    // Initialised in start(); stop() and the watchdog callback signal it to
    // unblock the WaitForSingleObject / eventfd_read in run().
    // std::optional<> because create() can fail (very rare OS error).
    std::optional<phyriad::hal::WakeEvent> wake_event{};

    // ── Tick counters ─────────────────────────────────────────────────────
    uint32_t power_check_counter{0u};
    static constexpr uint32_t kPowerCheckInterval = 10u;

    uint32_t self_monitor_counter{0u};
    static constexpr uint32_t kSelfMonitorInterval = 5u;
    /// E5 hardened coexistence full-process scan cadence (ticks). The scan is a
    /// full enumerate (~800µs @200 procs) so it runs periodically, not every tick.
    static constexpr uint32_t kCoexistScanInterval = 8u;

    uint32_t idle_check_counter{0u};
    static constexpr uint32_t kIdleCheckInterval = 20u;

    // Process enumeration cadence.
    // Tick interval in Active = 100 ms → refresh every 5 ticks = 500 ms.
    // Rationale: process lifecycle (Discord helpers spawning, game start/stop)
    // does not require 100 ms detection latency. 500 ms is plenty and cuts the
    // CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS) cost — which enumerates
    // ALL 200-400 system processes — by 5×. SelfMonitor measurements showed
    // refresh() was the #2 hot path (~2-3 ms per call). At 500 ms cadence,
    // average steady-state cost drops from ~25 ms/sec to ~5 ms/sec.
    //
    // Between refreshes, the cached targets_[] in ProcessObserver remains
    // valid; snapshot()/classify()/metrics-sample continue every tick.
    // Init to kProcessRefreshInterval so the first tick triggers refresh
    // immediately (no 500 ms blind window at startup before targets appear).
    uint32_t proc_refresh_counter{5u};
    static constexpr uint32_t kProcessRefreshInterval = 5u;

    // IPC command processing state.
    // policies_paused = true means executor.revert_all() has been called and
    // future apply() steps are skipped. UI sets via send_command(kPause).
    // Default: PAUSED. The agent never applies
    // optimisations on its own; the operator (UI Start button, or
    // `phynned-cli resume`, or the agent launched with --start-active) must
    // explicitly opt in. This prevents the surprise case where a user
    // tries Phynned out of curiosity while a kernel-anticheat game is
    // running and gets flagged by the AC for the OS-level affinity
    // changes Phynned would otherwise have made on startup.
    bool     policies_paused        {true};
    uint64_t last_cmd_seq_processed {0ull};

    // Foreground-heuristic auto-discovery state.
    // Runs every kAutoDiscoverInterval ticks. Tracks the last PID we already
    // attempted so we don't repeatedly probe the same foreground process via
    // OpenProcess + EnumProcessModules (which is expensive AND would look
    // noisy to any anti-cheat watching access patterns).
    uint32_t auto_discover_counter   {10u};   // init high → first tick checks
    static constexpr uint32_t kAutoDiscoverInterval = 10u;  // every ~1 second
    uint32_t last_auto_probed_pid    {0u};
    uint64_t last_auto_probed_tsc    {0ull};
    static constexpr uint64_t kAutoProbeReverifySec = 30ull; // re-probe same PID after 30s
};

// ── Constructor / Destructor ──────────────────────────────────────────────
AgentRuntime::AgentRuntime(AgentConfig cfg) noexcept
    : impl_(new (std::nothrow) Impl{})
{
    if (impl_) impl_->cfg = cfg;
}

AgentRuntime::~AgentRuntime() noexcept {
    if (impl_) {
        impl_->watchdog.stop();
        impl_->executor.revert_all();
        // wake_event is phyriad::hal::WakeEvent — RAII, no manual close needed.
        delete impl_;
    }
}

// ── start() ───────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error> AgentRuntime::start() noexcept {
    if (!impl_) return std::unexpected(phyriad::Error{phyriad::ErrorCode::OutOfMemory});

    // ── Memory budget ────────────────────────────────────────────────────
    apply_memory_budget();

    // ── TSC calibration ──────────────────────────────────────────────────
    impl_->tsc_freq = phyriad::hal::calibrate_tsc_freq();
    if (impl_->tsc_freq == 0u) impl_->tsc_freq = 2'000'000'000ull;

    // ── Privilege check ──────────────────────────────────────────────────
    {
        using phyriad::tuning::PrivilegeLevel;
        const auto lvl = phyriad::tuning::check_privilege_level();
        impl_->privilege_level = lvl;
        impl_->is_admin = (lvl >= PrivilegeLevel::Elevated);

        if (!impl_->is_admin) {
            if (impl_->cfg.require_admin) {
                std::fprintf(stderr,
                    "[Phynned] Admin required but not available. Exiting.\n");
                return std::unexpected(phyriad::Error{phyriad::ErrorCode::PermissionDenied});
            }
            std::fprintf(stdout,
                "[Phynned] WARNING: Not admin. ETW and affinity operations disabled.\n"
                "         Launch as Administrator for full functionality.\n");
        }
    }

    // ── Load user configuration (policies.toml) ───────────────────────────
    {
        auto cfg_result = config::ConfigStore::load_policies();
        if (cfg_result.has_value()) {
            impl_->agent_cfg = *cfg_result;
            std::fprintf(stdout, "[Phynned] Config loaded: op_mode=%u, %u rule overrides.\n",
                static_cast<unsigned>(impl_->agent_cfg.op_mode),
                impl_->agent_cfg.n_overrides);
        } else {
            std::fprintf(stdout, "[Phynned] No policies.toml found — using defaults.\n");
        }
    }

    // ── Load per-game memory (memory.toml) ────────────────────────────────
    {
        impl_->per_game.generate_hardware_id();
        (void)impl_->per_game.load();  // Non-fatal; empty table on first run.

        // Re-validation strategy (§8.3): expire entries older than 30 days.
        const uint32_t expired = impl_->per_game.expire_stale_entries(30u);

        std::fprintf(stdout,
            "[Phynned] Hardware: %s  Per-game: %u entries  Bad: %u  Expired: %u\n",
            impl_->per_game.hardware_id(),
            impl_->per_game.count(),
            impl_->per_game.bad_count(),
            expired);
    }

    // ── Hardware topology probe + self-pin ───────────────────────────────
    {
        const auto& topo = phyriad::hw::topology();
        if (impl_->cfg.self_pin_to_slow_cores) {
            apply_self_pin(topo);
        }
        impl_->policy_engine.register_default_rules(topo);

        // §9.2 AutoPolicySelector: classify CPU and pre-compute affinity masks.
        impl_->auto_selector.init_from_topology(topo);
        std::fprintf(stdout, "[Phynned] CPU class: %s\n",
            impl_->auto_selector.class_name());

        // §5.3 Default policy pack: write hardware-appropriate policies.toml
        // on first run so users can inspect / edit what Phynned will do.
        (void)config::DefaultPolicyPack::write_if_missing(topo);

        // ── MR-2 corral mask setup ────────────────────────────────────────
        // vcache = the V-Cache CCD cores (0x0000FFFF on the 7950X3D, from the
        // shadow router's cached hw::v_cache_cores()); system = all logical cores;
        // freq = the complement (0xFFFF0000) = the corral target. On a non-X3D box
        // vcache is 0 → freq is 0 → the corral degrades to a permanent no-op (E2).
        {
            const uint32_t n = topo.logical_core_count();
            impl_->system_core_mask = (n == 0u || n >= 32u)
                ? 0xFFFFFFFFu
                : ((1u << n) - 1u);
            impl_->vcache_core_mask = impl_->route_advisor.v_cache_mask();
            impl_->freq_corral_mask =
                static_cast<uint64_t>(impl_->system_core_mask
                                      & ~impl_->vcache_core_mask);
            std::fprintf(stdout,
                "[Phynned][CORRAL] masks: vcache=0x%08X freq=0x%08llX system=0x%08X\n",
                impl_->vcache_core_mask,
                static_cast<unsigned long long>(impl_->freq_corral_mask),
                impl_->system_core_mask);
        }
    }

    // ── Observation patterns ─────────────────────────────────────────────
    // ProcessObserver filters by case-insensitive substring match. Without
    // registered patterns it tracks 0 processes. Here we register the default
    // seed set: game runtimes (Java/LWJGL/.NET) + targets the classifier can
    // label as Game/Stream/Comm/Browser/Prod. ProcessClassifier decides
    // whether to act on each.
    {
        static constexpr const char* kSeedPatterns[] = {
            // ── Game runtimes (no D3D/Vulkan; el classifier los matchea
            // via OpenGL/LWJGL DLL detection) ─────────────────────────────
            "javaw",      // Minecraft Java, LWJGL games
            "java",       // launchers Java (Prism, MultiMC, ATLauncher)
            "Minecraft",  // Bedrock launcher + variantes (substring)

            // ── AAA games con executable names únicos (substring match) ──
            // Halo (Master Chief Collection y Halo Infinite)
            "MCC-Win64",        // Halo Master Chief Collection: MCC-Win64-Shipping.exe
            "HaloInfinite",     // Halo Infinite
            // Cyberpunk + Witcher (CD Projekt)
            "Cyberpunk",        // Cyberpunk2077.exe
            "witcher3",         // witcher3.exe
            // Bethesda / id-tech engines
            "Starfield",
            "Fallout",
            "Skyrim",
            "DOOMEternal", "doom",
            // Ubisoft (substring "Anvil"/"Snowdrop"/"Dunia" too vague)
            // ── Reservado para AAA mainstream sin anticheat agresivo ──
            "Hogwarts",         // Hogwarts Legacy
            "Stalker2",         // Stalker 2: Heart of Chornobyl
            "Spider-Man", "Marvel",
            "GodOfWar",
            "Returnal",
            "ELDEN",            // Elden Ring (caution: anticheat)
            "Forza",            // Forza Horizon (caution: online)
            // ── Unreal Engine generic ────────────────────────────────────
            "Win64-Shipping",   // Unreal-built games sin nombre específico
            "Shipping.exe",     // catch-all UE/UE5

            // ── miHoYo games ──────────────────────────
            // Gacha games

            "Honkai: Star Rail",

            // ── Legacy 32-bit games ──────────────────────────
            // Many pre-2015 games are 32-bit / x86. With FR-4 + the
            // EnumProcessModulesEx fix they can be auto-discovered, but a
            // direct name pattern is more reliable (auto-discovery requires
            // user to focus the window for ≥10s).
            "Borderlands",      // BL1/BL2/Pre-Sequel/BL3 (BL3 also matches Shipping)
            "BioShock",         // BioShock 1/2/Infinite
            "DeadSpace",        // Dead Space 1/2/3
            "MassEffect",       // Mass Effect original trilogy
            "Civ",              // Civilization V/VI (32-bit and 64-bit)
            "hl2.exe",          // Half-Life 2, Portal, TF2 (Source 32-bit, now 64)
            "csgo.exe",         // CS:GO (32-bit)
            "GTAIV", "GTAV",    // GTA IV (32) / GTA V (64)
            "Witcher2",         // The Witcher 2 (32-bit)
            // ── Anti-cheat protected + frame-gen helpers ─────────────
            // These never enter the observer via auto-discovery:
            //   - "League of Legends.exe" is Vanguard-protected →
            //     check_d3d_vk_modules() is denied PROCESS_VM_READ.
            //   - "LeagueClient*.exe" is CEF (classified Productivity).
            //   - "LosslessScaling.exe" is an overlay, never the foreground
            //     fullscreen window, so the auto-discovery gate skips it.
            // Substring patterns (no .exe) so we also catch future variants
            // such as a hypothetical "League of Legends 2.exe".
            "League of Legends",  // → kGameNames → Game (bypass heuristic)
            "LeagueClient",       // → kLauncherHelperNames → Productivity
            "LosslessScaling",    // → kGameNames → Game (bypass heuristic)
            "DiRT",             // DiRT Rally / DiRT 3 / etc.
            "ProjectCars",      // Project CARS
            "Dishonored",       // Dishonored 1/2

            // ── Stream apps (classifier → Stream → IsolateGame rule) ────
            "obs",          // obs64.exe, obs32.exe (substring)
            "Streamlabs",   // Streamlabs Desktop, Streamlabs OBS
            "XSplit",
            "Twitch",       // Twitch Studio
            "Lightstream",
            "Prism",        // Prism Live Studio

            // ── Comm apps ────────────────────────────────────────────────
            "Discord", "Teams", "ms-teams", "Zoom", "slack", "Skype",

            // ── Browsers (classifier → Browser, no action by default) ───
            "chrome", "firefox", "msedge", "brave", "opera",

            // ── Productivity (classifier → Productivity, observe-only) ──
            "devenv", "Code", "blender", "Resolve",
        };
        for (const char* p : kSeedPatterns) {
            impl_->observer.add_target_pattern(p);
        }
        std::fprintf(stdout,
            "[Phynned] Observer patterns registered: %zu seed entries.\n",
            sizeof(kSeedPatterns) / sizeof(kSeedPatterns[0]));
    }

    // ── Classification cache setup ───────────────────────────────────────
    {
        impl_->class_cache.set_classifier(&impl_->classifier);
        impl_->class_cache.set_tsc_freq(impl_->tsc_freq);
    }

    // ── Honour --start-active CLI flag ───────────────────────────────────
    // policies_paused defaults to true (safe-by-default). The agent
    // launcher / service registrar can set `cfg.start_active = true` to
    // skip the Start-button gate for headless deployments.
    if (impl_->cfg.start_active) {
        impl_->policies_paused = false;
        std::fprintf(stdout,
            "[Phynned] --start-active: policies APPLY at startup (no UI gate).\n");
    } else {
        std::fprintf(stdout,
            "[Phynned] Safe-default: policies PAUSED. Awaiting UI Start "
            "or `phynned-cli resume`.\n");
    }

    // ── MR-2 corral mode (safe-default DRY-RUN) ──────────────────────────
    // W2: the corral LIVE switch now PERSISTS across restarts via [corral] enabled.
    // corral_live = (file's [corral] enabled) OR the --corral-live self-test flag.
    // With no config file both are false → DRY-RUN, byte-identical to today.
    impl_->corral_live = impl_->agent_cfg.corral_enabled ||
                         impl_->cfg.corral_live_default;
    impl_->self_pid    = phyriad::proc::self_pid();  // never corral self (E6)
    std::fprintf(stdout,
        "[Phynned][CORRAL] background corral mode: %s (V-Cache CCD -> Freq CCD). "
        "profile=%u keep_on_disable=%d rules=%u\n",
        impl_->corral_live ? "LIVE (applies real affinity)"
                           : "DRY-RUN (default; nothing applied)",
        static_cast<unsigned>(impl_->agent_cfg.profile),
        impl_->agent_cfg.corral_keep_on_disable ? 1 : 0,
        impl_->agent_cfg.n_process_rules);

    // ── Manual TargetKind overrides (UI-editable file) ───────────────────
    // Load at startup; the agent main loop calls reload_if_changed() each
    // tick so users see their UI edits applied within ~100 ms without
    // having to restart the agent. classify() consults this table BEFORE
    // its built-in heuristics.
    {
        const uint32_t n_loaded = impl_->overrides.load();
        impl_->classifier.set_overrides(&impl_->overrides);
        if (n_loaded > 0u) {
            std::fprintf(stdout,
                "[Phynned] Loaded %u manual kind override(s).\n", n_loaded);
        }
    }

    // ── AutoRevertGuard setup ────────────────────────────────────────────
    {
        impl_->revert_guard.set_dependencies(
            &impl_->executor,
            &impl_->per_game,
            impl_->tsc_freq);
    }

    // ── MetricsCollector (ETW) — dynamic tier (§4.5) ────────────────────
    // Admin presence → start at Full tier; non-admin → stay at None.
    if (impl_->is_admin) {
        impl_->etw_mgr.force_tier(
            observer::EtwProviderSet::Full, impl_->metrics);
    }
    impl_->etw_active = impl_->etw_mgr.etw_active();

    // ── Wake event (FR-16) ────────────────────────────────────────────────
    // Auto-reset event; signal() in stop()/watchdog wakes the run() loop early.
    impl_->wake_event = phyriad::hal::WakeEvent::create();
    if (!impl_->wake_event.has_value()) {
        std::fprintf(stderr, "[Phynned] WakeEvent::create() failed — fatal.\n");
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError});
    }

    // ── Persistent audit log (§9.1) ──────────────────────────────────────
    {
        char audit_path[512]{};
        if (config::ConfigStore::get_config_dir(audit_path, sizeof(audit_path) - 16u)) {
            std::strncat(audit_path, "audit.bin", 10u);
            const uint32_t agent_pid = phyriad::proc::self_pid();  // FR-18
            (void)impl_->audit_log.open(audit_path, agent_pid);
        }
    }

    // ── SHM publisher ────────────────────────────────────────────────────
    if (impl_->cfg.enable_shm_publish) {
        const uint32_t agent_pid = phyriad::proc::self_pid();  // FR-18
        auto pub_result = impl_->publisher.open(impl_->cfg.shm_name, agent_pid);
        if (!pub_result) {
            std::fprintf(stderr,
                "[Phynned] WARNING: SHM publish failed — UI clients will not connect.\n");
            // Non-fatal: agent continues without UI
        } else {
            // Publish detected hardware classification (one-shot; static for
            // the lifetime of the agent). Lets the UI render arch-aware text
            // (X3D / HybridIntel / MultiCCXNoX3D / SingleCCD) without indirect
            // inference from telemetry counters.
            const auto& topo = phyriad::hw::topology();
            const auto pcores = phyriad::hw::p_cores();
            const auto ecores = phyriad::hw::e_cores();
            uint32_t max_ccd = 0u;
            for (const auto& c : topo.cores)
                if (c.ccd_id > max_ccd) max_ccd = c.ccd_id;
            const uint32_t n_ccd = topo.cores.empty() ? 0u : max_ccd + 1u;
            impl_->publisher.set_hw_classification(
                static_cast<uint8_t>(impl_->auto_selector.cpu_class()),
                static_cast<uint8_t>(n_ccd > 255u ? 255u : n_ccd),
                static_cast<uint8_t>(pcores.size() > 255u ? 255u : pcores.size()),
                static_cast<uint8_t>(ecores.size() > 255u ? 255u : ecores.size())
            );
        }
        // Publish the initial paused state so the UI knows whether to
        // show "Start" (paused, the safe default) or "Pause" (active,
        // when launched with --start-active or by a service installer
        // that resumes us at boot).
        impl_->publisher.set_policies_paused(impl_->policies_paused ? 1u : 0u);
        // W4: publish the loaded profile so the UI radio shows the right mode
        // immediately (before the first tick's per-tick republish).
        impl_->publisher.set_profile(
            static_cast<uint8_t>(impl_->agent_cfg.profile));
    }

    // ── Internal watchdog ────────────────────────────────────────────────
    impl_->watchdog.start(impl_->tsc_freq, [this]() noexcept {
        // Recovery: request stop so the run() loop exits cleanly.
        if (impl_) {
            phyriad::hal::seq_store_release(impl_->stop_requested, true);
            if (impl_->wake_event.has_value()) impl_->wake_event->signal();
        }
    });

    phyriad::hal::seq_store_release(impl_->running, true);
    std::fprintf(stdout, "[Phynned] Agent started. Admin=%s ETW=%s TSC=%.1f GHz\n",
        impl_->is_admin ? "yes" : "no",
        impl_->etw_active ? "yes" : "no",
        static_cast<double>(impl_->tsc_freq) / 1e9);

    return {};
}

// ── run() — main blocking loop ────────────────────────────────────────────
void AgentRuntime::run() noexcept {
    if (!impl_ || !phyriad::hal::seq_load_acquire(impl_->running)) return;

    while (!phyriad::hal::seq_load_acquire(impl_->stop_requested)) {
        // ── Watchdog heartbeat ────────────────────────────────────────────
        impl_->watchdog.heartbeat();

        // ── Tick start ────────────────────────────────────────────────────
        const uint32_t tick_n =
            phyriad::hal::stat_fetch_add_relaxed(impl_->tick_count, 1u) + 1u;
        phyriad::hal::stat_store_relaxed(diag::g_last_tick, tick_n);
        PHYNNED_PHASE(diag::PhaseTickStart);
        const uint64_t now_tsc = phyriad::hal::rdtsc();

        // ── IPC command processing ─────────────────
        PHYNNED_PHASE(diag::PhaseIpcCommand);
        // Poll the SHM command slot. If a new command arrived (seq changed),
        // dispatch it and bump ack so the client can observe completion.
        if (impl_->publisher.is_open()) {
            ipc::PhynnedCommandSlot* cmd_slot = impl_->publisher.command_slot();
            if (cmd_slot) {
                const uint64_t cur_seq =
                    phyriad::hal::seq_load_acquire(cmd_slot->seq);
                if (cur_seq != impl_->last_cmd_seq_processed) {
                    const uint32_t kind = cmd_slot->cmd_kind;
                    switch (kind) {
                        case ipc::kPhynnedCmdPausePolicies:
                            impl_->executor.revert_all();
                            impl_->policies_paused = true;
                            impl_->publisher.set_policies_paused(1u);
                            std::fprintf(stdout,
                                "[Phynned][IPC] policies PAUSED (seq=%llu)\n",
                                static_cast<unsigned long long>(cur_seq));
                            break;
                        case ipc::kPhynnedCmdResumePolicies:
                            impl_->policies_paused = false;
                            impl_->publisher.set_policies_paused(0u);
                            std::fprintf(stdout,
                                "[Phynned][IPC] policies RESUMED (seq=%llu)\n",
                                static_cast<unsigned long long>(cur_seq));
                            break;
                        case ipc::kPhynnedCmdForceRevertAll:
                            impl_->executor.revert_all();
                            // Match the user expectation: "Reset" clears
                            // active policies AND leaves the agent paused
                            // so it doesn't immediately re-apply on the
                            // next tick. User clicks Start again to
                            // resume.
                            impl_->policies_paused = true;
                            impl_->publisher.set_policies_paused(1u);
                            std::fprintf(stdout,
                                "[Phynned][IPC] force-revert-all (seq=%llu)\n",
                                static_cast<unsigned long long>(cur_seq));
                            break;
                        case ipc::kPhynnedCmdSetDifferentialPin: {
                            // UI-controlled toggle.
                            // arg1 is bool (0/1); revert any active actions
                            // so the next policy cycle re-applies under the
                            // new mode cleanly (avoids mixed Rule 1 / Rule 7
                            // active state).
                            const bool enable = (cmd_slot->arg1 != 0ull);
                            const bool current =
                                impl_->policy_engine.differential_pin_enabled();
                            if (enable == current) {
                                // No-op: same state already active. Rapid
                                // checkbox toggles (or duplicate IPC sends)
                                // would otherwise trigger redundant
                                // revert_all() + re-apply churn, which races
                                // against the next tick's evaluation.
                                std::fprintf(stdout,
                                    "[Phynned][IPC] differential-pin already %s "
                                    "(seq=%llu, no-op)\n",
                                    enable ? "ENABLED" : "DISABLED",
                                    static_cast<unsigned long long>(cur_seq));
                            } else {
                                impl_->executor.revert_all();
                                impl_->policy_engine.set_differential_pin_enabled(enable);
                                std::fprintf(stdout,
                                    "[Phynned][IPC] differential-pin mode %s "
                                    "(seq=%llu, reverted active policies)\n",
                                    enable ? "ENABLED" : "DISABLED",
                                    static_cast<unsigned long long>(cur_seq));
                            }
                            break;
                        }
                        case ipc::kPhynnedCmdSetCorralLive: {
                            // MR-2/W2: flip the background-corral LIVE switch. arg1
                            // = 0 → DRY-RUN, 1 → LIVE. arg2 = keep_placements_on_
                            // disable (piggybacked). On a LIVE→DRY-RUN transition
                            // with keep=false we selectively revert the corral
                            // placements (R3); game pins + user pins are untouched.
                            // The switch state persists via [corral] enabled (W2).
                            const bool enable = (cmd_slot->arg1 != 0ull);
                            const bool keep   = (cmd_slot->arg2 != 0ull);
                            const bool was_live = impl_->corral_live;
                            impl_->agent_cfg.corral_keep_on_disable = keep;
                            uint32_t reverted = 0u;
                            if (was_live && !enable && !keep) {
                                reverted = impl_->executor.revert_by_rule_id(
                                    kCorralRuleId);
                            }
                            impl_->corral_live               = enable;
                            impl_->agent_cfg.corral_enabled  = enable;
                            (void)config::ConfigStore::save_policies(
                                impl_->agent_cfg);
                            std::fprintf(stdout,
                                "[Phynned][IPC] corral LIVE switch %s keep=%d "
                                "reverted=%u (seq=%llu)%s\n",
                                enable ? "ON" : "OFF", keep ? 1 : 0, reverted,
                                static_cast<unsigned long long>(cur_seq),
                                (enable && impl_->coexist_optimizer_present)
                                    ? " — but a coexistence optimizer is present, "
                                      "staying DRY-RUN (E5)"
                                    : "");
                            break;
                        }
                        case ipc::kPhynnedCmdSetProcessRule: {
                            // W3: create/update a per-process user rule. Agent
                            // resolves the exe name from its tracked state (or
                            // QueryFullProcessImageNameW), stores an empty path
                            // (name-only match; hand-edit the TOML to add a path).
                            const uint32_t pid =
                                cmd_slot->target_pid;
                            const uint8_t action =
                                static_cast<uint8_t>(cmd_slot->arg1 & 0xFFu);
                            char name[64]{};
                            bool resolved = false;
                            for (uint32_t j = 0u; j < impl_->n_targets; ++j) {
                                if (impl_->target_buf[j].pid == pid &&
                                    impl_->target_buf[j].name[0] != '\0') {
                                    std::snprintf(name, sizeof(name), "%s",
                                                  impl_->target_buf[j].name);
                                    resolved = true;
                                    break;
                                }
                            }
                            if (!resolved)
                                resolved = get_process_short_name(
                                    pid, name, sizeof(name));

                            if (resolved && action <= 2u) {
                                config::AgentConfig& ac = impl_->agent_cfg;
                                bool updated = false;
                                for (uint32_t j = 0u;
                                     j < ac.n_process_rules; ++j) {
#ifdef _WIN32
                                    const bool same =
                                        (_stricmp(ac.process_rules[j].name,
                                                  name) == 0) &&
                                        ac.process_rules[j].path[0] == '\0';
#else
                                    const bool same =
                                        (std::strcmp(ac.process_rules[j].name,
                                                     name) == 0) &&
                                        ac.process_rules[j].path[0] == '\0';
#endif
                                    if (same) {
                                        ac.process_rules[j].action = action;
                                        updated = true;
                                        break;
                                    }
                                }
                                if (!updated &&
                                    ac.n_process_rules <
                                        config::AgentConfig::kMaxProcessRules) {
                                    config::ProcessRule& r =
                                        ac.process_rules[ac.n_process_rules++];
                                    r = config::ProcessRule{};
                                    std::snprintf(r.name, sizeof(r.name),
                                                  "%s", name);
                                    r.path[0] = '\0';
                                    r.action  = action;
                                }
                                (void)config::ConfigStore::save_policies(ac);
                                ++impl_->user_rules_generation;
                                std::fprintf(stdout,
                                    "[Phynned][IPC] user rule %s: %s -> action=%u "
                                    "(seq=%llu)\n",
                                    updated ? "updated" : "added", name,
                                    static_cast<unsigned>(action),
                                    static_cast<unsigned long long>(cur_seq));
                            } else {
                                std::fprintf(stdout,
                                    "[Phynned][IPC] SetProcessRule ignored "
                                    "(pid=%u unresolved or bad action) seq=%llu\n",
                                    pid,
                                    static_cast<unsigned long long>(cur_seq));
                            }
                            break;
                        }
                        case ipc::kPhynnedCmdRemoveProcessRule: {
                            // W3: remove a user rule by SHM slot index. arg2 =
                            // rules-block generation; a stale generation means the
                            // table changed under the UI, so ignore the request.
                            const uint32_t slot =
                                static_cast<uint32_t>(cmd_slot->arg1);
                            const uint32_t gen =
                                static_cast<uint32_t>(cmd_slot->arg2);
                            config::AgentConfig& ac = impl_->agent_cfg;
                            if (gen == impl_->user_rules_generation &&
                                slot < ac.n_process_rules) {
                                // Capture the name BEFORE compaction: removing an
                                // Always rule also undoes the pins it caused (the
                                // operator's off-means-undo semantics), scoped by
                                // exe so pins from OTHER user rules survive. If a
                                // second same-name rule remains, the next tick
                                // simply re-applies per that rule (self-correcting).
                                char removed_name[64];
                                std::memcpy(removed_name,
                                            ac.process_rules[slot].name,
                                            sizeof(removed_name));
                                for (uint32_t j = slot;
                                     j + 1u < ac.n_process_rules; ++j) {
                                    ac.process_rules[j] =
                                        ac.process_rules[j + 1u];
                                }
                                --ac.n_process_rules;
                                ac.process_rules[ac.n_process_rules] =
                                    config::ProcessRule{};
                                (void)config::ConfigStore::save_policies(ac);
                                ++impl_->user_rules_generation;
                                const uint32_t nrev =
                                    impl_->executor.revert_by_rule_id(
                                        kUserRuleId, removed_name);
                                std::fprintf(stdout,
                                    "[Phynned][IPC] user rule removed slot=%u "
                                    "exe=%s (%u pin%s reverted) (seq=%llu)\n",
                                    slot, removed_name, nrev,
                                    nrev == 1u ? "" : "s",
                                    static_cast<unsigned long long>(cur_seq));
                            } else {
                                std::fprintf(stdout,
                                    "[Phynned][IPC] RemoveProcessRule ignored "
                                    "(stale gen=%u vs %u or slot=%u oob) seq=%llu\n",
                                    gen, impl_->user_rules_generation, slot,
                                    static_cast<unsigned long long>(cur_seq));
                            }
                            break;
                        }
                        case ipc::kPhynnedCmdSetProfile: {
                            // W4: set the global profile. Leaving the corral-active
                            // profile reverts corral placements (R3); switching to
                            // Monitor also reverts user pins (nothing stays placed).
                            const uint8_t pv =
                                static_cast<uint8_t>(cmd_slot->arg1 & 0xFFu);
                            if (pv <= 3u) {
                                const config::Profile old_p =
                                    impl_->agent_cfg.profile;
                                const config::Profile new_p =
                                    static_cast<config::Profile>(pv);
                                const config::Profile old_eff =
                                    effective_profile(old_p);
                                const config::Profile new_eff =
                                    effective_profile(new_p);
                                impl_->agent_cfg.profile = new_p;

                                uint32_t rev_c = 0u, rev_u = 0u;
                                if (new_eff == config::Profile::Monitor) {
                                    rev_c = impl_->executor.revert_by_rule_id(
                                        kCorralRuleId);
                                    rev_u = impl_->executor.revert_by_rule_id(
                                        kUserRuleId);
                                } else if (old_eff ==
                                               config::Profile::GamesCorral &&
                                           new_eff !=
                                               config::Profile::GamesCorral) {
                                    rev_c = impl_->executor.revert_by_rule_id(
                                        kCorralRuleId);
                                }
                                (void)config::ConfigStore::save_policies(
                                    impl_->agent_cfg);
                                impl_->publisher.set_profile(pv);
                                std::fprintf(stdout,
                                    "[Phynned][IPC] profile %u->%u "
                                    "(corral_reverted=%u user_reverted=%u)%s "
                                    "(seq=%llu)\n",
                                    static_cast<unsigned>(old_p),
                                    static_cast<unsigned>(new_p), rev_c, rev_u,
                                    (new_p == config::Profile::Full)
                                        ? " [Full reserved (MR-3) -> games_corral "
                                          "behaviour]" : "",
                                    static_cast<unsigned long long>(cur_seq));
                            }
                            break;
                        }
                        default:
                            // Unknown — ignore but still ack so client doesn't retry.
                            break;
                    }
                    impl_->last_cmd_seq_processed = cur_seq;
                    phyriad::hal::seq_store_release(cmd_slot->ack, cur_seq);
                }
            }
        }

        // ── W4 placement master-gate (R6) ─────────────────────────────────
        // Computed AFTER IPC processing so a SetProfile command this tick takes
        // effect immediately. Monitor = observe/advise only: NO placement at any
        // of the three feed points (§3c-user user pins, §3c corral, §6 auto
        // apply). Observation / shadow / telemetry are NEVER gated by this.
        const config::Profile eff_profile =
            effective_profile(impl_->agent_cfg.profile);
        const bool placement_allowed =
            (eff_profile != config::Profile::Monitor);

        // ── Power check (every kPowerCheckInterval ticks) ─────────────────
        PHYNNED_PHASE(diag::PhasePowerCheck);
        if (++impl_->power_check_counter >= Impl::kPowerCheckInterval) {
            impl_->power_check_counter = 0u;
            impl_->power_watch.refresh();
        }

        // ── Idle check → DeepIdle transition ─────────────────────────────
        PHYNNED_PHASE(diag::PhaseIdleCheck);
        if (++impl_->idle_check_counter >= Impl::kIdleCheckInterval) {
            impl_->idle_check_counter = 0u;
            if (impl_->n_targets == 0u && impl_->idle_watch.desktop_idle_5min()) {
                impl_->workload_state = WorkloadState::DeepIdle;
            }
        }

        // ── ForegroundWatcher tick ────────────────────────────────────────
        PHYNNED_PHASE(diag::PhaseForegroundTick);
        const uint32_t sleep_ms_prev = tick_interval_ms(
            impl_->workload_state,
            impl_->power_watch.on_battery());
        impl_->fg_watcher.on_tick(sleep_ms_prev);

        // ── Foreground-heuristic auto-discovery ────────
        // The seed pattern list in ProcessObserver covers ~40 well-known exe
        // names. Anything else (Unity games with unique names, indie titles,
        // less-mainstream AAA) is INVISIBLE to Phynned because the observer
        // filters by pattern before the classifier ever sees the process.
        //
        // Fix: every kAutoDiscoverInterval ticks, examine the foreground PID.
        // If it's fullscreen + uses D3D/Vulkan/OpenGL (clear game signal) and
        // not already observed, register its exe name as a dynamic pattern.
        // Next refresh picks it up and it enters the normal classification
        // pipeline.
        //
        // Probe gate: we cache the last-probed PID + TSC so we don't re-probe
        // the same foreground PID every second. Only re-probe after 30 s OR
        // when the foreground PID changes (alt-tab to a different process).
        PHYNNED_PHASE(diag::PhaseAutoDiscover);
        if (++impl_->auto_discover_counter >= Impl::kAutoDiscoverInterval) {
            impl_->auto_discover_counter = 0u;
            const uint32_t fg_pid = impl_->fg_watcher.foreground_pid();
            if (fg_pid != 0u && fg_pid != 4u) {
                // Skip if same PID was probed within reverify window.
                const uint64_t tsc_now = phyriad::hal::rdtsc();
                const uint64_t age_tsc = tsc_now - impl_->last_auto_probed_tsc;
                const uint64_t reverify_tsc =
                    impl_->tsc_freq * Impl::kAutoProbeReverifySec;
                const bool same_pid_recent =
                    (fg_pid == impl_->last_auto_probed_pid) &&
                    (age_tsc < reverify_tsc);

                if (!same_pid_recent) {
                    // Skip if already in observed target_buf (no need to discover).
                    bool already_tracked = false;
                    for (uint32_t i = 0u; i < impl_->n_targets; ++i) {
                        if (impl_->target_buf[i].pid == fg_pid) {
                            already_tracked = true;
                            break;
                        }
                    }
                    if (!already_tracked
                        && impl_->fg_watcher.is_foreground_fullscreen())
                    {
                        // check_d3d_vk_modules opens process with PROCESS_VM_READ —
                        // strongest access right used by Phynned. Anti-cheat-protected
                        // games may deny this; we silently skip those.
                        const bool is_game = impl_->classifier
                            .check_d3d_vk_modules(fg_pid);
                        impl_->last_auto_probed_pid = fg_pid;
                        impl_->last_auto_probed_tsc = tsc_now;

                        if (is_game) {
                            char exe_name[64]{};
                            if (get_process_short_name(fg_pid, exe_name,
                                                        sizeof(exe_name))) {
                                // explorer.exe (and other
                                // OS shell processes) pass the fullscreen
                                // + D3D heuristic when the desktop is
                                // foreground, but they're not games.
                                // Silently skip to avoid polluting the
                                // observer pattern table.
                                if (!is_os_shell_process(exe_name)) {
                                    impl_->observer.add_target_pattern(exe_name);
                                    std::fprintf(stdout,
                                        "[Phynned] Auto-discovered foreground game: "
                                        "%s (PID %u) — pattern registered\n",
                                        exe_name, fg_pid);
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── 1. Enumerate targets ──────────────────────────────────────────
        PHYNNED_PHASE(diag::PhaseProcessRefresh);
        // refresh() is the #2 hot path
        // (CreateToolhelp32Snapshot of 200-400 system processes, ~2-3 ms).
        // Throttled to every kProcessRefreshInterval ticks. snapshot() is cheap
        // (memcpy of targets_[]) and runs every tick so target_buf stays fresh.
        if (++impl_->proc_refresh_counter >= Impl::kProcessRefreshInterval) {
            impl_->proc_refresh_counter = 0u;
            impl_->observer.refresh();
        }
        impl_->n_targets = impl_->observer.snapshot(
            impl_->target_buf.data(), observer::kMaxTargets);
        phyriad::hal::stat_store_relaxed(impl_->active_targets_n, impl_->n_targets);

        // ── 1a. Hot-reload manual kind overrides if file changed ────────
        // Cheap stat() check; on mtime change the overrides table is
        // refreshed from disk. classify() picks up the new mapping on the
        // very next classification pass (driven by class_cache).
        if (impl_->overrides.reload_if_changed()) {
            // Invalidate the cached classifications so the new override
            // takes effect immediately, not after the cache entries expire.
            impl_->class_cache.clear();
        }

        // ── 1b. Prune stale actions whose targets are gone ──────────────
        // When a game / app exits its PID disappears from observer.snapshot
        // but ActionExecutor's active_[] table would still list the old
        // entry — the Actions tab and "X actions" status badge would
        // forever report stale counts. Sync the bookkeeping each tick:
        // any active action whose PID isn't in target_buf is dropped (the
        // process is dead, no syscall to make).
        {
            uint32_t live_pids[observer::kMaxTargets];
            for (uint32_t i = 0u; i < impl_->n_targets; ++i) {
                live_pids[i] = impl_->target_buf[i].pid;
            }
            (void)impl_->executor.prune_dead(live_pids, impl_->n_targets);
        }

        // ── 2. Sample metrics ─────────────────────────────────────────────
        PHYNNED_PHASE(diag::PhaseMetricsSample);
        if (impl_->n_targets > 0u) {
            uint32_t pids[observer::kMaxTargets];
            for (uint32_t i = 0u; i < impl_->n_targets; ++i)
                pids[i] = impl_->target_buf[i].pid;

            impl_->metrics.sample(pids, impl_->n_targets,
                                  impl_->metrics_buf.data());
        }

        // ── 3. Classify targets + update TargetProcess::kind ─────────────
        PHYNNED_PHASE(diag::PhaseClassify);
        // The classification loop builds ProcessInfo
        // per-target every tick (foreground watcher × 3 calls, strncpy,
        // check_d3d_vk_modules cache scan, classify_cached). With n=64 targets
        // × 10 ticks/sec that adds up — and target kinds change on the order
        // of seconds, not 100 ms. Tie classification cadence to the existing
        // proc_refresh_counter (every 5 ticks = 500 ms): new targets only
        // appear at refresh boundaries anyway, so re-classifying between
        // refreshes is redundant.
        //
        // CRITICAL: revert_guard.on_tick() still fires EVERY tick — it
        // monitors frame-time variance for auto-revert and needs frequent
        // updates from the (always-fresh) metrics_buf.
        const bool do_classify = (impl_->proc_refresh_counter == 0u);

        for (uint32_t i = 0u; i < impl_->n_targets; ++i) {
            observer::TargetProcess& tp = impl_->target_buf[i];
            const observer::TargetMetrics& tm = impl_->metrics_buf[i];

            // Skip if already confidently classified.
            if (tp.kind == observer::TargetKind::System) continue;

            if (do_classify) {
                // Build ProcessInfo for classification.
                observer::ProcessInfo pinfo{};
                pinfo.pid              = tp.pid;
                pinfo.gpu_usage_pct    = 0.0f;
                // Pass CPU% and thread count from MetricsCollector
                // (FR-11 bulk snapshot) as additional non-invasive classifier
                // signals. Robust against PROCESS_VM_READ denial on protected
                // processes (Hogwarts Legacy, some anti-tamper games).
                pinfo.cpu_usage_pct    = tm.cpu_usage_pct;
                pinfo.thread_count     = tm.observed_threads;
                // Single is_foreground() call — was called twice before.
                const bool is_fg       = impl_->fg_watcher.is_foreground(tp.pid);
                pinfo.foreground_for_sec = is_fg
                                         ? impl_->fg_watcher.foreground_for_sec()
                                         : 0u;
                pinfo.is_fullscreen    = is_fg &&
                                         impl_->fg_watcher.is_foreground_fullscreen();
                pinfo.uses_d3d_or_vk   = impl_->classifier.check_d3d_vk_modules(tp.pid);
                std::strncpy(pinfo.exe_name, tp.name, sizeof(pinfo.exe_name) - 1u);

                const observer::TargetKind kind =
                    impl_->class_cache.classify_cached(tp.pid, tp.name,
                                                        pinfo, now_tsc);
                tp.kind = kind;
            } else {
                // On non-classify ticks, recover
                // kind from cache. snapshot() each tick overwrites tp.kind
                // with Unknown from ProcessObserver::targets_[] (which never
                // gets the classified kind back-written). The classification
                // cache IS persistent across ticks, so a cheap O(n) lookup
                // by name restores the correct kind for display + policy.
                //
                // Without this: 4 of 5 ticks the kind appears as Unknown,
                // breaking the Targets panel UI and degrading rule matching.
                tp.kind = impl_->class_cache.lookup_kind(tp.pid, tp.name);
            }

            // AutoRevertGuard tick for active monitors — runs EVERY tick even
            // when classification is skipped (uses fresh metrics from
            // metrics_buf which is updated every tick).
            if (impl_->revert_guard.is_monitoring(tp.pid)) {
                impl_->revert_guard.on_tick(
                    tp.pid, tm.frame_time_variance_ms, now_tsc);
            }

            // Hot thread visibility log.
            // Per-target diagnostic: print when the identified "hot thread"
            // TID changes for a Game-classified target. Helps verify the
            // heuristic before we wire it into a thread-pin policy (M2).
            //
            // Cost: one fprintf per kind transition or hot_tid change per
            // Game target — very low volume.
            if (do_classify && tp.kind == observer::TargetKind::Game) {
                static uint32_t s_last_logged_hot_tid[observer::kMaxTargets]{};
                if (i < observer::kMaxTargets &&
                    tm.hot_tid != 0u &&
                    tm.hot_tid != s_last_logged_hot_tid[i])
                {
                    std::fprintf(stdout,
                        "[Phynned][HotThread] %-32s [pid=%u] "
                        "hot_tid=%u (was %u)\n",
                        tp.name, tp.pid,
                        tm.hot_tid, s_last_logged_hot_tid[i]);
                    s_last_logged_hot_tid[i] = tm.hot_tid;
                }
            }
        }

        // ── 3b. SHADOW ROUTER (MR-1) — advisory CCD recommendation ─────────
        // Read-only DECISION layer. For each observed process it writes a CCD
        // recommendation onto metrics_buf[i] (advice_ccd/advice_confidence) and
        // logs MOVE advice (M4a). It STRUCTURALLY CANNOT PLACE: RouteAdvisor::
        // advise() returns a pure value (RouteAdvice) with no reference to
        // policy_engine, decision_buf, or executor — the advice never enters the
        // §5 policy path, which still consumes ONLY the eligible subset. Runs over
        // the full tracked herd because its purpose is "this observed process WOULD
        // route to X"; games/eligible/system/idle are marked LeaveAlone inside
        // advise() and never produce a MOVE line.
        PHYNNED_PHASE(diag::PhaseClassify);
        {
            uint32_t n_logged = 0u;
            for (uint32_t i = 0u; i < impl_->n_targets; ++i) {
                const observer::TargetProcess& tp = impl_->target_buf[i];
                observer::TargetMetrics&       tm = impl_->metrics_buf[i];

                const bool game_managed =
                    impl_->observer.is_placement_eligible(tp.name);
                const learn::RouteAdvice adv =
                    impl_->route_advisor.advise(tm, tp.kind, game_managed);

                // Ride the existing SHM publish: these two bytes live inside
                // TargetMetrics and are copied into shm_metrics[] in §7 — no new
                // SHM field, no protocol size change.
                tm.advice_ccd        = static_cast<uint8_t>(adv.ccd);
                tm.advice_confidence = adv.confidence;

                // ── M4a: log MOVE advice, rate-limited on advice-change per pid ──
                const bool is_move =
                    (adv.ccd != learn::RouteAdvice::Ccd::LeaveAlone) &&
                    !adv.already_there;
                if (!is_move) continue;
                if (n_logged >= Impl::kMaxAdviceLogPerTick) continue;

                // Direct-mapped rate-limit cache (pid % slots): log only when this
                // pid's advice CHANGES. Collisions self-evict (overwrite) and at
                // worst cause an occasional duplicate line — bounded by the per-tick
                // cap above, so M3 is respected.
                const uint8_t ccd_u = static_cast<uint8_t>(adv.ccd);
                Impl::AdviceLogSlot& slot =
                    impl_->advice_log_cache[tp.pid % Impl::kAdviceLogSlots];
                const bool changed =
                    (slot.pid != tp.pid) || (slot.last_ccd != ccd_u);
                if (!changed) continue;
                slot.pid      = tp.pid;
                slot.last_ccd = ccd_u;

                const char* ccd_name =
                    (adv.ccd == learn::RouteAdvice::Ccd::VCache) ? "VCache" : "Freq";
                std::fprintf(stdout,
                    "[Phynned][SHADOW] pid=%u exe=%s advise=%s ws=%uMB "
                    "cpu=%.1f%% conf=%u (%s)\n",
                    tp.pid, tp.name[0] ? tp.name : "<unknown>",
                    ccd_name, tm.working_set_mb, tm.cpu_usage_pct,
                    adv.confidence, adv.reason);
                ++n_logged;
            }
        }

        // ── 3c-user. USER "ALWAYS" PINS (W3) — precedence #3, above auto ───
        // Runs BEFORE the corral so a user pin WINS over it (the corral then skips
        // any user-ruled process). Gated by the placement master-gate (Monitor →
        // no user placement, R6) and the pause gate. Per-rule status flags
        // (blocked_by_ac / flap_warn) are recomputed each tick for the UI badges.
        PHYNNED_PHASE(diag::PhaseClassify);
        {
            const uint32_t n_rules = impl_->agent_cfg.n_process_rules;
            // Clear the dynamic flags each tick (has_path is added at publish).
            for (uint32_t r = 0u; r < n_rules; ++r)
                impl_->user_rule_flags[r] = 0u;

            if (placement_allowed && !impl_->policies_paused && n_rules > 0u) {
                uint32_t n_logged = 0u;
                for (uint32_t i = 0u; i < impl_->n_targets; ++i) {
                    const observer::TargetProcess& tp = impl_->target_buf[i];
                    observer::TargetMetrics&       tm = impl_->metrics_buf[i];
                    if (tp.pid == impl_->self_pid) continue;  // never pin self (E6)

                    const config::ProcessRule* rule =
                        find_user_rule_for(impl_->agent_cfg, tp.name, tp.pid);
                    if (rule == nullptr) continue;
                    const uint32_t idx = static_cast<uint32_t>(
                        rule - impl_->agent_cfg.process_rules);
                    // Never (veto) is enforced by EXCLUSION in the corral + §6 auto
                    // passes; there is nothing to place here.
                    if (rule->action ==
                        static_cast<uint8_t>(config::RuleAction::Never))
                        continue;

                    const uint64_t our_mask =
                        impl_->executor.active_applied_mask(tp.pid);
                    const UserPinDecision upd = evaluate_user_pin(
                        rule->action, tp.kind, tm, our_mask,
                        impl_->vcache_core_mask, impl_->system_core_mask,
                        impl_->freq_corral_mask);

                    Impl::UserPinLogSlot& slot =
                        impl_->user_pin_log_cache[tp.pid % Impl::kAdviceLogSlots];

                    if (upd.already_placed) continue;

                    if (!upd.would_pin) {
                        // R4 flap guard: another manager owns this affinity.
                        if (upd.flap_warn) {
                            if (idx < n_rules)
                                impl_->user_rule_flags[idx] |=
                                    ipc::kUserRuleFlagFlapWarn;
                            const uint8_t st = 3u;  // flap
                            if ((slot.pid != tp.pid || slot.last_state != st) &&
                                n_logged < Impl::kMaxCorralLogPerTick) {
                                slot.pid = tp.pid; slot.last_state = st;
                                std::fprintf(stdout,
                                    "[Phynned][W3] user-pin SKIP pid=%u exe=%s "
                                    "cur=0x%08X — pinned by another manager "
                                    "(flap guard, never fight)\n",
                                    tp.pid, tp.name[0] ? tp.name : "<unknown>",
                                    tm.current_core_mask);
                                ++n_logged;
                            }
                        }
                        continue;
                    }

                    // ── R1: Game / unknown-class → SAME §6 AC probe/oracle gate.
                    // A Refused/Blocked verdict SKIPS the apply entirely: the
                    // game's process is never opened for a Set*. NEVER bypassed.
                    if (upd.needs_ac_gate) {
                        const action::ProbeResult pr =
                            action::AcProbe::probe_and_label(
                                tp.pid, tp.name, impl_->ac_oracle,
                                impl_->per_game,
                                impl_->audit_log.is_open()
                                    ? &impl_->audit_log : nullptr);
                        if (pr == action::ProbeResult::Refused_DoNotProbe ||
                            pr == action::ProbeResult::Blocked ||
                            pr == action::ProbeResult::AlreadyLabeledBlocked) {
                            if (idx < n_rules)
                                impl_->user_rule_flags[idx] |=
                                    ipc::kUserRuleFlagBlockedAc;
                            const uint8_t st = 4u;  // blocked
                            if ((slot.pid != tp.pid || slot.last_state != st) &&
                                n_logged < Impl::kMaxCorralLogPerTick) {
                                slot.pid = tp.pid; slot.last_state = st;
                                std::fprintf(stdout,
                                    "[Phynned][W3][R1] user-pin AC-gate SKIP "
                                    "pid=%u exe=%s verdict=%s (no placement)\n",
                                    tp.pid, tp.name,
                                    action::to_string(pr));
                                ++n_logged;
                            }
                            continue;   // never reaches executor.apply
                        }
                    }

                    // ── Apply via the EXISTING executor path (journal + revert).
                    policy::PolicyDecision d{};
                    d.core_mask      = upd.target_mask;
                    d.decided_tsc    = now_tsc;
                    d.target_pid     = tp.pid;
                    d.rule_id        = kUserRuleId;
                    d.priority_class = 0u;
                    d.action_kind    = policy::ActionKind::PinAffinity;
                    d.confidence     = 100u;
                    const auto ar = impl_->executor.apply(d, tp.name,
                                                          /*creation_time=*/0ull);
                    const uint8_t st = ar ? 2u : 1u;
                    if ((slot.pid != tp.pid || slot.last_state != st) &&
                        n_logged < Impl::kMaxCorralLogPerTick) {
                        slot.pid = tp.pid; slot.last_state = st;
                        std::fprintf(stdout,
                            "[Phynned][W3] user-pin pid=%u exe=%s ->0x%08llX %s "
                            "(%s)\n",
                            tp.pid, tp.name[0] ? tp.name : "<unknown>",
                            static_cast<unsigned long long>(upd.target_mask),
                            ar ? "applied" : "apply-failed", upd.reason);
                        ++n_logged;
                    }
                }
            }
        }

        // ── 3c. BACKGROUND CORRAL (MR-2) — DRY-RUN default ─────────────────
        // Moves ACTIVE non-game background off the V-Cache CCD onto the Freq CCD,
        // keeping the 96 MB V-Cache clean for games / measured cache-winners.
        //
        // SCOPE BOUNDARY: default DRY-RUN. It computes the CorralDecision, surfaces
        // it (a [CORRAL-DRYRUN] log line + the per-target advice_ccd=WouldCorral UI
        // marker) and applies NOTHING. The ONLY path to executor.apply is the
        // `corral_apply` branch below, gated behind the LIVE switch AND the E5
        // coexistence guard. evaluate_corral() is a pure value function with no
        // reference to the executor, so a dry-run decision is STRUCTURALLY unable to
        // place — exactly the RouteAdvisor guarantee, extended to the action layer.
        PHYNNED_PHASE(diag::PhaseClassify);
        {
            // E5 coexistence — HARDENED. A periodic FULL-process scan (every
            // kCoexistScanInterval ticks) catches an affinity optimizer that is
            // FILTERED OUT of the tracked/touchable set (e.g. the AMD 3D V-Cache
            // service, commonly denylisted from tracking) — the previous
            // tracked-only scan would miss it, letting the corral flap against it.
            // Runs on the first tick (counter starts 0) so the guard is armed
            // before the switch could ever be flipped.
            if (impl_->coexist_scan_counter == 0u) {
                static thread_local phyriad::proc::ProcessEntry
                    s_coexist[phyriad::proc::kMaxProcesses];
                const uint32_t nn = phyriad::proc::enumerate_processes(
                    s_coexist, phyriad::proc::kMaxProcesses);
                bool full = false;
                for (uint32_t i = 0u; i < nn; ++i) {
                    if (is_coexistence_optimizer(s_coexist[i].name)) { full = true; break; }
                }
                impl_->coexist_full_scan_result = full;
            }
            if (++impl_->coexist_scan_counter >= Impl::kCoexistScanInterval)
                impl_->coexist_scan_counter = 0u;

            // Full-scan verdict (may be up to kCoexistScanInterval ticks stale) OR
            // the per-tick tracked scan (immediate for a tracked optimizer).
            bool coexist = impl_->coexist_full_scan_result;
            for (uint32_t i = 0u; !coexist && i < impl_->n_targets; ++i) {
                if (is_coexistence_optimizer(impl_->target_buf[i].name))
                    coexist = true;
            }
            impl_->coexist_optimizer_present = coexist;

            // The ONLY gate that can route a corral decision into executor.apply.
            // corral_live starts false (safe default); coexistence forces false;
            // the corral runs ONLY in the GamesCorral profile and never in Monitor
            // (R6). placement_allowed is redundant with the GamesCorral check but
            // kept explicit to mirror the master-gate at every feed point.
            const bool corral_apply =
                impl_->corral_live && !coexist && placement_allowed &&
                eff_profile == config::Profile::GamesCorral;

            uint32_t n_logged = 0u;
            for (uint32_t i = 0u; i < impl_->n_targets; ++i) {
                const observer::TargetProcess& tp = impl_->target_buf[i];
                observer::TargetMetrics&       tm = impl_->metrics_buf[i];

                // Never corral the agent itself (E6 self-exclusion): apply_self_pin
                // pins only the agent's main THREAD to a non-V-Cache core, so its
                // PROCESS mask stays full and would otherwise match the predicate.
                if (tp.pid == impl_->self_pid) continue;

                // W3 precedence: any user rule (Never veto OR Always pin) wins over
                // the corral. Always is placed by the §3c-user pass; Never means
                // do-not-touch. Either way the corral does not touch this process.
                if (find_user_rule_for(impl_->agent_cfg, tp.name, tp.pid)
                        != nullptr)
                    continue;

                const bool eligible =
                    impl_->observer.is_placement_eligible(tp.name);
                const CorralDecision cd = evaluate_corral(
                    tp, tm, eligible,
                    impl_->vcache_core_mask, impl_->system_core_mask,
                    impl_->freq_corral_mask);
                if (!cd.would_corral) continue;

                // Per-target UI marker — rides the existing advice_ccd byte into
                // the SHM (no layout change): "the corral WOULD move this to Freq".
                tm.advice_ccd = observer::kAdviceCcdWouldCorral;

                // Rate-limit the log to a would-corral state CHANGE per pid (same
                // bounded direct-mapped table pattern as the shadow router).
                Impl::CorralLogSlot& slot =
                    impl_->corral_log_cache[tp.pid % Impl::kAdviceLogSlots];
                const uint8_t state_now = corral_apply ? 2u : 1u; // 1=dry,2=live
                const bool changed =
                    (slot.pid != tp.pid) || (slot.last_state != state_now);

                if (!corral_apply) {
                    // ── DRY-RUN: surface only. ZERO Set*; executor NEVER reached ──
                    if (changed && n_logged < Impl::kMaxCorralLogPerTick) {
                        slot.pid = tp.pid; slot.last_state = state_now;
                        std::fprintf(stdout,
                            "[Phynned][CORRAL-DRYRUN] pid=%u exe=%s would->Freq"
                            "(0x%08llX) cpu=%.1f%% cur=0x%08X ws=%uMB (%s)\n",
                            tp.pid, tp.name[0] ? tp.name : "<unknown>",
                            static_cast<unsigned long long>(cd.target_mask),
                            tm.cpu_usage_pct, tm.current_core_mask,
                            tm.working_set_mb, cd.reason);
                        ++n_logged;
                    }
                    continue;  // ← the structural no-op: dry-run stops here
                }

                // ── LIVE: reuse the EXISTING executor.apply (journal + revert;
                // the §6 AC-gate is moot here — corral candidates are non-Game by
                // predicate). No second Set* path. ────────────────────────────
                policy::PolicyDecision d{};
                d.core_mask      = cd.target_mask;
                d.decided_tsc    = now_tsc;
                d.target_pid     = tp.pid;
                d.rule_id        = kCorralRuleId;
                d.priority_class = 0u;
                d.action_kind    = policy::ActionKind::PinAffinity;
                d.confidence     = 100u;
                const auto ar =
                    impl_->executor.apply(d, tp.name, /*creation_time=*/0ull);
                if (changed && n_logged < Impl::kMaxCorralLogPerTick) {
                    slot.pid = tp.pid; slot.last_state = state_now;
                    std::fprintf(stdout,
                        "[Phynned][CORRAL-LIVE] pid=%u exe=%s ->Freq(0x%08llX) "
                        "%s (%s)\n",
                        tp.pid, tp.name[0] ? tp.name : "<unknown>",
                        static_cast<unsigned long long>(cd.target_mask),
                        ar ? "applied" : "apply-failed", cd.reason);
                    ++n_logged;
                }
            }
        }

        // ── 4. Classify workload state ────────────────────────────────────
        PHYNNED_PHASE(diag::PhaseWorkloadState);
        if (impl_->n_targets == 0u) {
            // Maintain DeepIdle if idle was already triggered; otherwise Idle.
            if (impl_->workload_state != WorkloadState::DeepIdle) {
                impl_->workload_state = WorkloadState::Idle;
            }
        } else {
            // Reset DeepIdle once a target appears.
            impl_->workload_state = WorkloadState::Active;
        }

        // ── 4b. Dynamic ETW tier update (§4.5) ───────────────────────────
        PHYNNED_PHASE(diag::PhaseEtwTierUpdate);
        if (impl_->is_admin) {
            const observer::EtwProviderSet desired =
                observer::provider_set_for(impl_->workload_state);
            impl_->etw_mgr.on_workload_changed(desired, impl_->metrics);
            impl_->etw_active = impl_->etw_mgr.etw_active();
        }

        // ── 5. Evaluate policies ──────────────────────────────────────────
        PHYNNED_PHASE(diag::PhasePolicyEvaluate);
        // ── MASS-router PLACEMENT BOUNDARY (2026-07-17) ───────────────────
        // Detection now tracks ALL touchable processes, but PLACEMENT must stay
        // exactly as before: only pattern-ELIGIBLE processes may be policy-
        // evaluated. Build that subset here and feed it (NOT the full target_buf)
        // to every policy path. This is what guarantees a newly-observed mass
        // process (Unknown/Productivity herd) NEVER reaches executor.apply —
        // including via Rule 8 (CCD Load Defense), which selects non-Game targets
        // by CPU% and would otherwise evict arbitrary background processes off the
        // V-Cache CCD. The real mass-routing policy is a separate later task.
        impl_->n_eligible = 0u;
        for (uint32_t i = 0u; i < impl_->n_targets; ++i) {
            if (impl_->observer.is_placement_eligible(impl_->target_buf[i].name)) {
                impl_->eligible_targets[impl_->n_eligible] = impl_->target_buf[i];
                impl_->eligible_metrics[impl_->n_eligible] = impl_->metrics_buf[i];
                ++impl_->n_eligible;
            }
        }
        if (impl_->n_eligible > 0u) {
            // ── 5a. AutoPolicySelector: per-game memory shortcut (§9.2/9.3) ─
            // For targets with a fresh PerGameMemory entry, inject decisions
            // directly from the cache, bypassing PolicyEngine for those targets.
            // bit i = target[i] had a per-game-memory hit. std::bitset (not a
            // uint32_t) because n_targets ranges to kMaxTargets(64): the old
            // `1u << i` was undefined behaviour for i>=32 (a pre-existing bug,
            // fixed 2026-07-17 BR1). bitset<64> is stack-only, no heap.
            std::bitset<observer::kMaxTargets> memory_hit_mask;
            impl_->n_decisions = 0u;

            if (impl_->auto_selector.is_ready()) {
                for (uint32_t i = 0u; i < impl_->n_eligible &&
                        impl_->n_decisions < policy::kMaxDecisionsPerCycle; ++i)
                {
                    const observer::TargetProcess& tp = impl_->eligible_targets[i];
                    if (impl_->per_game.is_bad(tp.name)) continue;

                    const policy::AutoDecision ad = impl_->auto_selector.select(
                        tp.kind, tp.name, &impl_->per_game);

                    if (ad.from_memory && ad.core_mask != 0ull) {
                        // Memory hit: synthesize decision with high confidence.
                        memory_hit_mask.set(i);
                        auto& d          = impl_->decision_buf[impl_->n_decisions++];
                        d.target_pid     = tp.pid;
                        d.rule_id        = 99u; // sentinel: memory-sourced
                        d.action_kind    = ad.action_kind;
                        d.confidence     = ad.confidence;
                        d._pad[0]        = 0u;
                        d._pad[1]        = 0u;
                        d.core_mask      = ad.core_mask;
                        d.priority_class = 0u;
                        d.decided_tsc    = now_tsc;
                    }
                }
            }

            // ── 5b. PolicyEngine: rule-based evaluation for remaining targets
            // Targets that got a memory hit are filtered out to avoid duplicates.
            // NOTE: evaluated over the pattern-ELIGIBLE subset only (n_eligible),
            // never the full touchable set — the MASS placement boundary.
            if (memory_hit_mask.none()) {
                // Common case: no memory hits — evaluate all eligible targets.
                impl_->n_decisions = impl_->policy_engine.evaluate(
                    impl_->eligible_targets.data(),  impl_->n_eligible,
                    impl_->eligible_metrics.data(), impl_->n_eligible,
                    impl_->decision_buf.data());
            } else {
                // Partial case: filter out memory-hit targets, evaluate the rest.
                // filtered_* are Impl members (off-stack at the MASS cap).
                auto& filtered_targets = impl_->filtered_targets;
                auto& filtered_metrics = impl_->filtered_metrics;
                uint32_t n_filtered = 0u;

                for (uint32_t i = 0u; i < impl_->n_eligible; ++i) {
                    if (memory_hit_mask.test(i)) continue; // already handled
                    filtered_targets[n_filtered] = impl_->eligible_targets[i];
                    filtered_metrics[n_filtered] = impl_->eligible_metrics[i];
                    ++n_filtered;
                }

                if (n_filtered > 0u) {
                    policy::PolicyDecision extra_buf[policy::kMaxDecisionsPerCycle]{};
                    const uint32_t n_extra = impl_->policy_engine.evaluate(
                        filtered_targets.data(), n_filtered,
                        filtered_metrics.data(),  n_filtered,
                        extra_buf);

                    for (uint32_t k = 0u; k < n_extra &&
                            impl_->n_decisions < policy::kMaxDecisionsPerCycle; ++k)
                    {
                        impl_->decision_buf[impl_->n_decisions++] = extra_buf[k];
                    }
                }
            }

            // ── 6. Apply decisions ────────────────────────────────────────
            PHYNNED_PHASE(diag::PhaseApplyDecisions);
            // Skip apply when policies are paused via IPC.
            // PolicyEngine still evaluates (so UI can see what WOULD be
            // applied), but executor.apply() is skipped. Existing actions
            // were reverted at pause time.
            for (uint32_t i = 0u; i < impl_->n_decisions; ++i) {
                // R6: Monitor profile never applies (evaluation still populated
                // decision_buf above, so the UI still sees what WOULD apply).
                if (impl_->policies_paused || !placement_allowed) break;
                const policy::PolicyDecision& d = impl_->decision_buf[i];

                // Diag: record which decision we're about to apply.
                diag::g_last_apply_pid .store(d.target_pid,
                                              std::memory_order_relaxed);  // HAL: relaxed — secondary atomic in compound op
                phyriad::hal::stat_store_relaxed(diag::g_last_apply_rule, d.rule_id);

                // Skip if this exe is on the bad list.
                bool skip = false;
                for (uint32_t j = 0u; j < impl_->n_targets && !skip; ++j) {
                    if (impl_->target_buf[j].pid == d.target_pid) {
                        skip = impl_->per_game.is_bad(impl_->target_buf[j].name);
                    }
                }
                if (skip) continue;

                // ── Look up this decision's target descriptor ────────────────
                // Needed by the CR1 AC-gate (exe name + kind) and the DR1 journal
                // plumbing (exe name). Decisions derive from target_buf, so a
                // match is expected; tp==nullptr degrades safely (no gate; apply
                // self-queries the exe).
                const observer::TargetProcess* tp = nullptr;
                for (uint32_t j = 0u; j < impl_->n_targets; ++j) {
                    if (impl_->target_buf[j].pid == d.target_pid) {
                        tp = &impl_->target_buf[j];
                        break;
                    }
                }

                // ── W3 precedence: a user rule overrides auto placement ──────
                // Never = veto; Always = already handled by the §3c-user pass.
                // Either way the automatic path (game rule / memory / A/B) must
                // not act on a user-ruled process (precedence: user > auto).
                if (tp != nullptr &&
                    find_user_rule_for(impl_->agent_cfg, tp->name, tp->pid)
                        != nullptr) {
                    continue;
                }

                // ── CR1: anti-cheat gate — BEFORE any OpenProcess / set ──────
                // Runs before executor.apply AND apply_differential_pin for any
                // GAME target. AcProbe opens ZERO handles on a do-not-probe title
                // (SilentPunish_C / UnknownAc) and short-circuits with no handle
                // on a prior per-exe label; steady-state cost ≈ 0 (one probe per
                // exe-identity, EVER). A Refused/Blocked verdict SKIPS the apply
                // entirely — the game's process is never opened (M4a: logged).
                if (tp != nullptr && tp->kind == observer::TargetKind::Game) {
                    const action::ProbeResult pr = action::AcProbe::probe_and_label(
                        d.target_pid,
                        tp->name,
                        impl_->ac_oracle,
                        impl_->per_game,
                        impl_->audit_log.is_open() ? &impl_->audit_log : nullptr);
                    if (pr == action::ProbeResult::Refused_DoNotProbe ||
                        pr == action::ProbeResult::Blocked            ||
                        pr == action::ProbeResult::AlreadyLabeledBlocked) {
                        std::fprintf(stdout,
                            "[Phynned][CR1] AC-gate SKIP: pid=%u exe=%s verdict=%s "
                            "(no placement applied)\n",
                            d.target_pid, tp->name, action::to_string(pr));
                        continue;   // never reaches any executor.apply* for this PID
                    }
                }

                // Rule 7 (PinHotThreadDifferential)
                // requires a side channel to ActionExecutor because the
                // hot_tid isn't part of the IPC-stable PolicyDecision struct.
                // We look it up from metrics_buf using target_pid.
                std::expected<void, phyriad::Error> apply_result{};
                if (d.rule_id ==
                    policy::PolicyEngine::kRuleIdPinHotThreadDifferential)
                {
                    uint32_t hot_tid = 0u;
                    for (uint32_t j = 0u; j < impl_->n_targets; ++j) {
                        if (impl_->target_buf[j].pid == d.target_pid) {
                            hot_tid = impl_->metrics_buf[j].hot_tid;
                            break;
                        }
                    }
                    if (hot_tid != 0u) {
                        // Differential: process gets FULL system mask
                        // (workers free); hot thread gets V-Cache mask.
                        phyriad::hal::stat_store_relaxed(diag::g_last_apply_tid, hot_tid);
                        apply_result = impl_->executor.apply_differential_pin(
                            d.target_pid,
                            hot_tid,
                            /*process_mask=*/~0ull,
                            /*thread_mask=*/d.core_mask,
                            policy::PolicyEngine::kRuleIdPinHotThreadDifferential);
                    } else {
                        // No hot_tid yet (confidence gate hasn't fired).
                        // Skip this tick — PolicyEngine will retry next cycle.
                        continue;
                    }
                } else {
                    // DR1: plumb the exe basename into the write-ahead journal.
                    // TargetProcess carries no process creation-time, so pass 0
                    // and let ActionExecutor self-query it (via GetProcessTimes)
                    // for the pid-recycle-safe journal key.
                    apply_result = impl_->executor.apply(
                        d, (tp != nullptr) ? tp->name : nullptr, /*creation_time=*/0ull);
                }

                if (apply_result) {
                    // Policy applied — start monitoring with AutoRevertGuard.
                    for (uint32_t j = 0u; j < impl_->n_targets; ++j) {
                        if (impl_->target_buf[j].pid == d.target_pid) {
                            const float var = impl_->metrics_buf[j].frame_time_variance_ms;
                            impl_->revert_guard.on_policy_applied(
                                d.target_pid,
                                impl_->target_buf[j].name,
                                var,
                                now_tsc);
                            break;
                        }
                    }
                }
            }
        }

        // ── 6b. Persist new action entries to audit.bin (§9.1) ───────────
        PHYNNED_PHASE(diag::PhaseAuditDrain);
        if (impl_->audit_log.is_open()) {
            impl_->audit_log.drain_and_write(
                impl_->executor.action_log(),
                &impl_->audit_cursor);
        }

        // ── 6c. Fix A: reconcile allowed_core_mask from live executor state ──
        // MetricsCollector::sample() zeroes allowed_core_mask every tick, so we
        // stamp it here from the executor's active table: a PID with an active
        // placement gets the applied mask (UI "routed"/green affinity); a PID
        // whose action was reverted/pruned gets 0 (cleared). This makes the UI
        // signal reflect live executor state without the executor holding a
        // metrics pointer. Covers game placements AND live corral placements.
        for (uint32_t i = 0u; i < impl_->n_targets; ++i) {
            impl_->metrics_buf[i].allowed_core_mask =
                static_cast<uint32_t>(
                    impl_->executor.active_applied_mask(impl_->target_buf[i].pid)
                    & 0xFFFFFFFFull);
        }

        // ── 7. Publish to SHM ─────────────────────────────────────────────
        PHYNNED_PHASE(diag::PhaseShmPublish);
        if (impl_->publisher.is_open()) {
            // ── Select the bounded top-N view for the SHM (UI contract) ──────
            // MASS-router: detection tracks up to kMaxTargets (1024) internally,
            // but the agent↔UI SHM stays kMaxShmTargets (64). Publish only the most
            // "interesting" slice: placement-eligible processes (games + the curated
            // set the UI has always shown) rank first, then by CPU% activity. The
            // full herd is tracked internally but never crosses the SHM boundary.
            const uint32_t n_all = impl_->n_targets;
            uint32_t order[observer::kMaxTargets];
            for (uint32_t i = 0u; i < n_all; ++i) order[i] = i;
            const auto shm_score = [&](uint32_t i) noexcept -> double {
                const observer::TargetProcess& tp = impl_->target_buf[i];
                const observer::TargetMetrics& tm = impl_->metrics_buf[i];
                double s = static_cast<double>(tm.cpu_usage_pct);
                if (tp.kind == observer::TargetKind::Game) s += 5.0e5;
                if (impl_->observer.is_placement_eligible(tp.name)) s += 1.0e6;
                return s;
            };
            std::sort(order, order + n_all,
                [&](uint32_t a, uint32_t b) noexcept {
                    return shm_score(a) > shm_score(b);
                });
            const uint32_t n_pub = std::min<uint32_t>(n_all,
                                                      observer::kMaxShmTargets);

            // Aggregate stats over the PUBLISHED set (matches SHM n_targets; not
            // diluted by the hundreds of idle herd processes now observed).
            uint32_t total_migrations = 0u;
            float    total_pressure   = 0.0f;
            for (uint32_t k = 0u; k < n_pub; ++k) {
                const uint32_t i = order[k];
                impl_->shm_targets[k] = impl_->target_buf[i];
                impl_->shm_metrics[k] = impl_->metrics_buf[i];
                total_migrations += impl_->metrics_buf[i].migrations_per_sec;
                total_pressure   += static_cast<float>(
                    impl_->metrics_buf[i].pressure_level) / 2.0f;
            }
            const float aggregate_pressure =
                (n_pub > 0u)
                    ? (total_pressure / static_cast<float>(n_pub))
                    : 0.0f;

            // Publish the real PrivilegeLevel enum value (0=None,
            // 1=Partial, 2=Elevated, 3=Admin) so the UI shows the correct
            // tier. ETW availability is published as a separate bit below
            // (don't squash it into priv_level — that was the old encoding
            // that made admin processes look like "no privileges").
            const uint8_t priv_level =
                static_cast<uint8_t>(impl_->privilege_level);

            const uint8_t is_deep_idle =
                (impl_->workload_state == WorkloadState::DeepIdle) ? 1u : 0u;

            impl_->publisher.publish(
                priv_level,
                impl_->etw_active ? 1u : 0u,
                impl_->shm_targets.data(),
                n_pub,
                impl_->shm_metrics.data(),
                impl_->decision_buf.data(),
                impl_->n_decisions,
                impl_->executor.active_count(),
                total_migrations,
                aggregate_pressure,
                now_tsc,
                impl_->executor.action_log(),
                impl_->per_game.bad_count(),
                is_deep_idle,
                1u /* watchdog_ok — always 1; watchdog sets stop_requested on stall */,
                // CCD Load Defense telemetry
                impl_->policy_engine.last_ccd_defense_count(),
                impl_->policy_engine.last_ccd_defense_cpu_pct(),
                // MASS-router: full internal tracked count (the herd), of which
                // only n_pub (≤ kMaxShmTargets) crossed into the SHM above.
                n_all);

            // ── MR-2: publish the background-corral mode (DRY-RUN / LIVE) ──
            // corral_coexist_block reflects the E5 guard measured this tick.
            impl_->publisher.set_corral_mode(
                impl_->corral_live ? 1u : 0u,
                impl_->coexist_optimizer_present ? 1u : 0u);

            // ── W3/W4: publish the profile + the per-process user rules table ─
            impl_->publisher.set_profile(
                static_cast<uint8_t>(impl_->agent_cfg.profile));
            {
                const uint32_t nr = impl_->agent_cfg.n_process_rules;
                for (uint32_t r = 0u; r < nr; ++r) {
                    const config::ProcessRule& pr =
                        impl_->agent_cfg.process_rules[r];
                    ipc::UserRuleShm& u = impl_->shm_user_rules[r];
                    std::memset(&u, 0, sizeof(u));
                    std::snprintf(u.name, sizeof(u.name), "%s", pr.name);
                    std::snprintf(u.path, sizeof(u.path), "%s", pr.path);
                    u.action = pr.action;
                    u.flags  = impl_->user_rule_flags[r];
                    if (pr.path[0] != '\0')
                        u.flags |= ipc::kUserRuleFlagHasPath;
                }
                impl_->publisher.publish_user_rules(
                    impl_->shm_user_rules.data(), nr,
                    impl_->user_rules_generation);
            }
        }

        // ── Self-monitor (every kSelfMonitorInterval ticks) ───────────────
        PHYNNED_PHASE(diag::PhaseSelfMonitor);
        if (++impl_->self_monitor_counter >= Impl::kSelfMonitorInterval) {
            impl_->self_monitor_counter = 0u;
            const float tick_ms = static_cast<float>(tick_interval_ms(
                impl_->workload_state,
                impl_->power_watch.on_battery()));
            impl_->self_monitor.sample(impl_->workload_state, tick_ms);
            // Publish to SHM so the UI status bar can show real numbers
            // instead of the placeholder 0% / 0 MB the old code emitted.
            const auto& sm = impl_->self_monitor.metrics();
            impl_->publisher.set_self_resources(
                sm.cpu_pct,
                static_cast<float>(sm.rss_bytes) / (1024.0f * 1024.0f));
        }

        // ── Adaptive sleep (FR-16) ────────────────────────────────────────
        PHYNNED_PHASE(diag::PhaseWakeWait);
        // WakeEvent::wait() blocks until timeout OR stop()/watchdog signals it.
        // Auto-reset: the signal is consumed by this wait, no manual reset.
        const uint32_t sleep_ms = tick_interval_ms(
            impl_->workload_state,
            impl_->power_watch.on_battery());

        (void)impl_->wake_event->wait(sleep_ms);
    }

    // ── Shutdown: signal UI clients, then revert actions ─────────────────
    impl_->publisher.close();   // marks agent_connected=0 before unmapping
    impl_->executor.revert_all();
    // Flush any remaining audit entries after revert_all() added them
    if (impl_->audit_log.is_open()) {
        impl_->audit_log.drain_and_write(
            impl_->executor.action_log(), &impl_->audit_cursor);
        impl_->audit_log.close();
    }
    // Stop ETW via manager (graceful tier transition → None)
    impl_->etw_mgr.force_tier(observer::EtwProviderSet::None, impl_->metrics);
    impl_->watchdog.stop();

    // Evict DLL cache for all targets (clean shutdown).
    for (uint32_t i = 0u; i < impl_->n_targets; ++i) {
        impl_->classifier.evict_pid(impl_->target_buf[i].pid);
    }

    // Persist per-game memory on clean shutdown.
    (void)impl_->per_game.save();

    phyriad::hal::seq_store_release(impl_->running, false);

    std::fprintf(stdout, "[Phynned] Agent stopped after %u ticks.\n",
        phyriad::hal::stat_load_relaxed(impl_->tick_count));
}

// ── stop() ────────────────────────────────────────────────────────────────
void AgentRuntime::stop() noexcept {
    if (!impl_) return;
    phyriad::hal::seq_store_release(impl_->stop_requested, true);
    // Unblock run()'s WakeEvent::wait() immediately — safe from any thread.
    if (impl_->wake_event.has_value()) impl_->wake_event->signal();
}

// ── Introspection ─────────────────────────────────────────────────────────
bool AgentRuntime::running() const noexcept {
    return impl_ && phyriad::hal::seq_load_acquire(impl_->running);
}
WorkloadState AgentRuntime::workload_state() const noexcept {
    return impl_ ? impl_->workload_state : WorkloadState::DeepIdle;
}
uint32_t AgentRuntime::tick_count() const noexcept {
    return impl_ ? phyriad::hal::stat_load_relaxed(impl_->tick_count) : 0u;
}
uint32_t AgentRuntime::active_targets() const noexcept {
    return impl_ ? phyriad::hal::stat_load_relaxed(impl_->active_targets_n) : 0u;
}
bool AgentRuntime::is_admin() const noexcept {
    return impl_ && impl_->is_admin;
}
bool AgentRuntime::etw_active() const noexcept {
    return impl_ && impl_->etw_active;
}

} // namespace phynned::core
// Made with my soul - Swately <3
