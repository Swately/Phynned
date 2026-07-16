// apps/ayama/core/src/AgentRuntime.cpp
// AgentRuntime — implementation.
//

#include <ayama/core/AgentRuntime.hpp>
#include <ayama/core/AdaptiveTick.hpp>
#include <ayama/core/Diag.hpp>
#include <ayama/core/SelfMonitor.hpp>
#include <ayama/core/PowerWatch.hpp>
#include <ayama/core/AutoRevertGuard.hpp>
#include <ayama/core/InternalWatchdog.hpp>
#include <ayama/core/IdleWatch.hpp>
#include <ayama/observer/ProcessObserver.hpp>
#include <ayama/observer/MetricsCollector.hpp>
#include <ayama/observer/ProcessClassifier.hpp>
#include <ayama/observer/KindOverrides.hpp>
#include <ayama/observer/ForegroundWatcher.hpp>
#include <ayama/observer/ClassificationCache.hpp>
#include <ayama/observer/EtwProviderSet.hpp>
#include <ayama/policy/PolicyEngine.hpp>
#include <ayama/policy/AutoPolicySelector.hpp>
#include <ayama/action/ActionExecutor.hpp>
#include <ayama/action/AuditLog.hpp>
#include <ayama/bench/ABRunner.hpp>
#include <ayama/config/ConfigStore.hpp>
#include <ayama/config/DefaultPolicyPack.hpp>
#include <ayama/learn/PerGameMemory.hpp>
#include <ayama/ipc/AyamaAgentPublisher.hpp>

#include <phyriad/topology/HardwareTopology.hpp>
#include <phyriad/tuning/PrivilegeCheck.hpp>
#include <phyriad/tuning/WorkingSet.hpp>   // FR-19 — set_self_working_set
#include <phyriad/hal/Timestamp.hpp>
#include <phyriad/hal/WakeEvent.hpp>       // FR-16 — cross-platform wake event
#include <phyriad/process/CurrentProcess.hpp> // FR-18 — self_pid / self_name

#include <array>
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

namespace ayama::core {

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
#define AYAMA_PHASE(P) \
    ::ayama::core::diag::g_last_phase.store(static_cast<uint32_t>(P), \
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
//   [Ayama] Auto-discovered foreground game: explorer.exe (PID 2692) — pattern registered
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
                "[Ayama] Self-pinned to E-core %u (logical)\n", c.logical_id);
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
                    "[Ayama] Self-pinned to non-V-Cache core %u\n", c.logical_id);
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
            "[Ayama] Self-pinned to last core %u (fallback)\n", last);
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
            "[Ayama] Working-set hint skipped (code=%u) — continuing.\n",
            static_cast<unsigned>(r.error().code));
    }
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

    // ── A/B benchmark runner ──────────────────────────────────────────────
    bench::ABRunner                 ab_runner{};   // controlled via IPC commands

    // ── SHM publisher ─────────────────────────────────────────────────────
    ipc::AyamaAgentPublisher        publisher{};   // write side of shared memory

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
    // `ayama-cli resume`, or the agent launched with --start-active) must
    // explicitly opt in. This prevents the surprise case where a user
    // tries Ayama out of curiosity while a kernel-anticheat game is
    // running and gets flagged by the AC for the OS-level affinity
    // changes Ayama would otherwise have made on startup.
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
                    "[Ayama] Admin required but not available. Exiting.\n");
                return std::unexpected(phyriad::Error{phyriad::ErrorCode::PermissionDenied});
            }
            std::fprintf(stdout,
                "[Ayama] WARNING: Not admin. ETW and affinity operations disabled.\n"
                "         Launch as Administrator for full functionality.\n");
        }
    }

    // ── Load user configuration (policies.toml) ───────────────────────────
    {
        auto cfg_result = config::ConfigStore::load_policies();
        if (cfg_result.has_value()) {
            impl_->agent_cfg = *cfg_result;
            std::fprintf(stdout, "[Ayama] Config loaded: op_mode=%u, %u rule overrides.\n",
                static_cast<unsigned>(impl_->agent_cfg.op_mode),
                impl_->agent_cfg.n_overrides);
        } else {
            std::fprintf(stdout, "[Ayama] No policies.toml found — using defaults.\n");
        }
    }

    // ── Load per-game memory (memory.toml) ────────────────────────────────
    {
        impl_->per_game.generate_hardware_id();
        (void)impl_->per_game.load();  // Non-fatal; empty table on first run.

        // Re-validation strategy (§8.3): expire entries older than 30 days.
        const uint32_t expired = impl_->per_game.expire_stale_entries(30u);

        std::fprintf(stdout,
            "[Ayama] Hardware: %s  Per-game: %u entries  Bad: %u  Expired: %u\n",
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
        std::fprintf(stdout, "[Ayama] CPU class: %s\n",
            impl_->auto_selector.class_name());

        // §5.3 Default policy pack: write hardware-appropriate policies.toml
        // on first run so users can inspect / edit what Ayama will do.
        (void)config::DefaultPolicyPack::write_if_missing(topo);
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
            "[Ayama] Observer patterns registered: %zu seed entries.\n",
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
            "[Ayama] --start-active: policies APPLY at startup (no UI gate).\n");
    } else {
        std::fprintf(stdout,
            "[Ayama] Safe-default: policies PAUSED. Awaiting UI Start "
            "or `ayama-cli resume`.\n");
    }

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
                "[Ayama] Loaded %u manual kind override(s).\n", n_loaded);
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
        std::fprintf(stderr, "[Ayama] WakeEvent::create() failed — fatal.\n");
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
                "[Ayama] WARNING: SHM publish failed — UI clients will not connect.\n");
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
    std::fprintf(stdout, "[Ayama] Agent started. Admin=%s ETW=%s TSC=%.1f GHz\n",
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
        AYAMA_PHASE(diag::PhaseTickStart);
        const uint64_t now_tsc = phyriad::hal::rdtsc();

        // ── IPC command processing ─────────────────
        AYAMA_PHASE(diag::PhaseIpcCommand);
        // Poll the SHM command slot. If a new command arrived (seq changed),
        // dispatch it and bump ack so the client can observe completion.
        if (impl_->publisher.is_open()) {
            ipc::AyamaCommandSlot* cmd_slot = impl_->publisher.command_slot();
            if (cmd_slot) {
                const uint64_t cur_seq =
                    phyriad::hal::seq_load_acquire(cmd_slot->seq);
                if (cur_seq != impl_->last_cmd_seq_processed) {
                    const uint32_t kind = cmd_slot->cmd_kind;
                    switch (kind) {
                        case ipc::kAyamaCmdPausePolicies:
                            impl_->executor.revert_all();
                            impl_->policies_paused = true;
                            impl_->publisher.set_policies_paused(1u);
                            std::fprintf(stdout,
                                "[Ayama][IPC] policies PAUSED (seq=%llu)\n",
                                static_cast<unsigned long long>(cur_seq));
                            break;
                        case ipc::kAyamaCmdResumePolicies:
                            impl_->policies_paused = false;
                            impl_->publisher.set_policies_paused(0u);
                            std::fprintf(stdout,
                                "[Ayama][IPC] policies RESUMED (seq=%llu)\n",
                                static_cast<unsigned long long>(cur_seq));
                            break;
                        case ipc::kAyamaCmdForceRevertAll:
                            impl_->executor.revert_all();
                            // Match the user expectation: "Reset" clears
                            // active policies AND leaves the agent paused
                            // so it doesn't immediately re-apply on the
                            // next tick. User clicks Start again to
                            // resume.
                            impl_->policies_paused = true;
                            impl_->publisher.set_policies_paused(1u);
                            std::fprintf(stdout,
                                "[Ayama][IPC] force-revert-all (seq=%llu)\n",
                                static_cast<unsigned long long>(cur_seq));
                            break;
                        case ipc::kAyamaCmdSetDifferentialPin: {
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
                                    "[Ayama][IPC] differential-pin already %s "
                                    "(seq=%llu, no-op)\n",
                                    enable ? "ENABLED" : "DISABLED",
                                    static_cast<unsigned long long>(cur_seq));
                            } else {
                                impl_->executor.revert_all();
                                impl_->policy_engine.set_differential_pin_enabled(enable);
                                std::fprintf(stdout,
                                    "[Ayama][IPC] differential-pin mode %s "
                                    "(seq=%llu, reverted active policies)\n",
                                    enable ? "ENABLED" : "DISABLED",
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

        // ── Power check (every kPowerCheckInterval ticks) ─────────────────
        AYAMA_PHASE(diag::PhasePowerCheck);
        if (++impl_->power_check_counter >= Impl::kPowerCheckInterval) {
            impl_->power_check_counter = 0u;
            impl_->power_watch.refresh();
        }

        // ── Idle check → DeepIdle transition ─────────────────────────────
        AYAMA_PHASE(diag::PhaseIdleCheck);
        if (++impl_->idle_check_counter >= Impl::kIdleCheckInterval) {
            impl_->idle_check_counter = 0u;
            if (impl_->n_targets == 0u && impl_->idle_watch.desktop_idle_5min()) {
                impl_->workload_state = WorkloadState::DeepIdle;
            }
        }

        // ── ForegroundWatcher tick ────────────────────────────────────────
        AYAMA_PHASE(diag::PhaseForegroundTick);
        const uint32_t sleep_ms_prev = tick_interval_ms(
            impl_->workload_state,
            impl_->power_watch.on_battery());
        impl_->fg_watcher.on_tick(sleep_ms_prev);

        // ── Foreground-heuristic auto-discovery ────────
        // The seed pattern list in ProcessObserver covers ~40 well-known exe
        // names. Anything else (Unity games with unique names, indie titles,
        // less-mainstream AAA) is INVISIBLE to Ayama because the observer
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
        AYAMA_PHASE(diag::PhaseAutoDiscover);
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
                        // strongest access right used by Ayama. Anti-cheat-protected
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
                                        "[Ayama] Auto-discovered foreground game: "
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
        AYAMA_PHASE(diag::PhaseProcessRefresh);
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
        AYAMA_PHASE(diag::PhaseMetricsSample);
        if (impl_->n_targets > 0u) {
            uint32_t pids[observer::kMaxTargets];
            for (uint32_t i = 0u; i < impl_->n_targets; ++i)
                pids[i] = impl_->target_buf[i].pid;

            impl_->metrics.sample(pids, impl_->n_targets,
                                  impl_->metrics_buf.data());
        }

        // ── 3. Classify targets + update TargetProcess::kind ─────────────
        AYAMA_PHASE(diag::PhaseClassify);
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
                        "[Ayama][HotThread] %-32s [pid=%u] "
                        "hot_tid=%u (was %u)\n",
                        tp.name, tp.pid,
                        tm.hot_tid, s_last_logged_hot_tid[i]);
                    s_last_logged_hot_tid[i] = tm.hot_tid;
                }
            }
        }

        // ── 4. Classify workload state ────────────────────────────────────
        AYAMA_PHASE(diag::PhaseWorkloadState);
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
        AYAMA_PHASE(diag::PhaseEtwTierUpdate);
        if (impl_->is_admin) {
            const observer::EtwProviderSet desired =
                observer::provider_set_for(impl_->workload_state);
            impl_->etw_mgr.on_workload_changed(desired, impl_->metrics);
            impl_->etw_active = impl_->etw_mgr.etw_active();
        }

        // ── 5. Evaluate policies ──────────────────────────────────────────
        AYAMA_PHASE(diag::PhasePolicyEvaluate);
        if (impl_->n_targets > 0u) {
            // ── 5a. AutoPolicySelector: per-game memory shortcut (§9.2/9.3) ─
            // For targets with a fresh PerGameMemory entry, inject decisions
            // directly from the cache, bypassing PolicyEngine for those targets.
            uint32_t memory_hit_mask = 0u;    // bit i = target[i] had memory hit
            impl_->n_decisions = 0u;

            if (impl_->auto_selector.is_ready()) {
                for (uint32_t i = 0u; i < impl_->n_targets &&
                        impl_->n_decisions < policy::kMaxDecisionsPerCycle; ++i)
                {
                    const observer::TargetProcess& tp = impl_->target_buf[i];
                    if (impl_->per_game.is_bad(tp.name)) continue;

                    const policy::AutoDecision ad = impl_->auto_selector.select(
                        tp.kind, tp.name, &impl_->per_game);

                    if (ad.from_memory && ad.core_mask != 0ull) {
                        // Memory hit: synthesize decision with high confidence.
                        memory_hit_mask |= (1u << i);
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
            if (memory_hit_mask == 0u) {
                // Common case: no memory hits — evaluate all targets normally.
                impl_->n_decisions = impl_->policy_engine.evaluate(
                    impl_->target_buf.data(),  impl_->n_targets,
                    impl_->metrics_buf.data(), impl_->n_targets,
                    impl_->decision_buf.data());
            } else {
                // Partial case: filter out memory-hit targets, evaluate the rest.
                observer::TargetProcess  filtered_targets[observer::kMaxTargets]{};
                observer::TargetMetrics  filtered_metrics[observer::kMaxTargets]{};
                uint32_t n_filtered = 0u;

                for (uint32_t i = 0u; i < impl_->n_targets; ++i) {
                    if (memory_hit_mask & (1u << i)) continue; // already handled
                    filtered_targets[n_filtered] = impl_->target_buf[i];
                    filtered_metrics[n_filtered] = impl_->metrics_buf[i];
                    ++n_filtered;
                }

                if (n_filtered > 0u) {
                    policy::PolicyDecision extra_buf[policy::kMaxDecisionsPerCycle]{};
                    const uint32_t n_extra = impl_->policy_engine.evaluate(
                        filtered_targets, n_filtered,
                        filtered_metrics,  n_filtered,
                        extra_buf);

                    for (uint32_t k = 0u; k < n_extra &&
                            impl_->n_decisions < policy::kMaxDecisionsPerCycle; ++k)
                    {
                        impl_->decision_buf[impl_->n_decisions++] = extra_buf[k];
                    }
                }
            }

            // ── 6. Apply decisions ────────────────────────────────────────
            AYAMA_PHASE(diag::PhaseApplyDecisions);
            // Skip apply when policies are paused via IPC.
            // PolicyEngine still evaluates (so UI can see what WOULD be
            // applied), but executor.apply() is skipped. Existing actions
            // were reverted at pause time.
            for (uint32_t i = 0u; i < impl_->n_decisions; ++i) {
                if (impl_->policies_paused) break;
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
                    apply_result = impl_->executor.apply(d);
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
        AYAMA_PHASE(diag::PhaseAuditDrain);
        if (impl_->audit_log.is_open()) {
            impl_->audit_log.drain_and_write(
                impl_->executor.action_log(),
                &impl_->audit_cursor);
        }

        // ── 7. Publish to SHM ─────────────────────────────────────────────
        AYAMA_PHASE(diag::PhaseShmPublish);
        if (impl_->publisher.is_open()) {
            // Compute aggregate stats from metrics_buf.
            uint32_t total_migrations = 0u;
            float    total_pressure   = 0.0f;
            for (uint32_t i = 0u; i < impl_->n_targets; ++i) {
                total_migrations += impl_->metrics_buf[i].migrations_per_sec;
                total_pressure   += static_cast<float>(
                    impl_->metrics_buf[i].pressure_level) / 2.0f;
            }
            const float aggregate_pressure =
                (impl_->n_targets > 0u)
                    ? (total_pressure / static_cast<float>(impl_->n_targets))
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
                impl_->target_buf.data(),
                impl_->n_targets,
                impl_->metrics_buf.data(),
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
                impl_->policy_engine.last_ccd_defense_cpu_pct());
        }

        // ── Self-monitor (every kSelfMonitorInterval ticks) ───────────────
        AYAMA_PHASE(diag::PhaseSelfMonitor);
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
        AYAMA_PHASE(diag::PhaseWakeWait);
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

    std::fprintf(stdout, "[Ayama] Agent stopped after %u ticks.\n",
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

} // namespace ayama::core
// Made with my soul - Swately <3
