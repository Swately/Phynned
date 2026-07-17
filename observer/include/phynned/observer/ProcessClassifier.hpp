// observer/include/phynned/observer/ProcessClassifier.hpp
// ProcessClassifier — heuristic process classification into TargetKind.
//
// Rules (conservative — when in doubt, return Unknown):
//   1. System guard:    known-OS exe names → always System (never touched).
//   2. Exact name match: OBS/Teams/Chrome/… → Stream/Comm/Browser.
//   3. Game heuristics: requires ≥3 of 4 signals to agree:
//        a) is_fullscreen (ForegroundWatcher)
//        b) gpu_usage_pct > 25%
//        c) foreground_for_sec > 30
//        d) uses_d3d_or_vk (EnumProcessModules cache)
//   4. Productivity:    devenv/blender/Resolve/… → Productivity (no Auto action).
//   5. Default:         Unknown → no action in Auto mode.
//
// Threading: single-thread (agent main thread). check_d3d_vk_modules() is
//            cached per PID — O(1) after first call per unique PID.
// Resource:  ~4 KB for DLL-cache table; no heap after init.
// Privilege: EnumProcessModules requires PROCESS_QUERY_INFORMATION — usually
//            available without admin for non-protected processes.
//
#pragma once

#include <phynned/observer/TargetProcess.hpp>
#include <phyriad/stigmergy/Classifier.hpp>   // §P-0.5.3 — stigmergic interface
#include <cstdint>
#include <cstring>
#include <array>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace phynned::observer {

// ── ProcessInfo — input to classify() ──────────────────────────────────────
/// All signals needed for classification. Built each tick from observer + metrics.
///
/// Mayo 2026 extension: added `cpu_usage_pct` and `thread_count` so the
/// classifier can detect games even when graphics-DLL inspection fails (e.g.
/// access-restricted processes). These signals come "for free" from
/// MetricsCollector via FR-11 bulk capture — already collected for every
/// observed target.
struct alignas(8) ProcessInfo {
    uint32_t pid;                  //  4B  @ 0
    uint32_t foreground_for_sec;   //  4B  @ 4   — seconds this PID has been foreground
    char     exe_name[40];         // 40B  @ 8   — null-terminated short exe name
    float    gpu_usage_pct;        //  4B  @ 48  — 0..100, 0 if unknown (PDH TODO)
    float    cpu_usage_pct;        //  4B  @ 52  — 0..100×N_cores; from TargetMetrics
    uint32_t thread_count;         //  4B  @ 56  — observed thread count
    bool     is_fullscreen;        //  1B  @ 60  — window covers ≥95% of monitor
    bool     uses_d3d_or_vk;       //  1B  @ 61  — d3d/vulkan/opengl DLL loaded
    uint8_t  _pad[2];              //  2B  @ 62  — fill to 64B
};                                 // = 64B
static_assert(sizeof(ProcessInfo) == 64u);

// ── Known-name tables (null-terminated arrays) ──────────────────────────────

/// Streaming encoders (high confidence).
inline constexpr const char* kStreamNames[] = {
    "obs64.exe",              "obs32.exe",
    "obs.exe",
    "Streamlabs OBS.exe",     "Streamlabs Desktop.exe",
    "XSplit.Core.exe",        "Twitch Studio.exe",
    "Lightstream.exe",        "Prism Live Studio.exe",
    nullptr,
};

/// Communication apps.
inline constexpr const char* kCommNames[] = {
    "Discord.exe",    "DiscordCanary.exe",   "DiscordPTB.exe",
    "Teams.exe",      "ms-teams.exe",
    "Zoom.exe",       "ZoomPhone.exe",
    "slack.exe",
    "Skype.exe",      "SkypeApp.exe",
    "Telegram.exe",   "Signal.exe",
    nullptr,
};

/// Web browsers.
inline constexpr const char* kBrowserNames[] = {
    "chrome.exe",    "chromium.exe",
    "firefox.exe",
    "msedge.exe",    "MicrosoftEdge.exe",
    "brave.exe",
    "opera.exe",     "opera_gx.exe",
    "vivaldi.exe",   "whale.exe",
    nullptr,
};

/// Productivity / creative tools (observed but no Auto action by default).
inline constexpr const char* kProductivityNames[] = {
    "devenv.exe",           // Visual Studio
    "Code.exe",             // VS Code
    "blender.exe",
    "Resolve.exe",          // DaVinci Resolve
    "premiere.exe",
    "AfterFX.exe",          // After Effects
    "Photoshop.exe",        "Illustrator.exe",
    "maya.exe",             "3dsmax.exe",
    "cinema4d.exe",
    nullptr,
};

/// Explicit Game names. Classified as Game without running the fullscreen /
/// CPU / threads heuristic gates. Use sparingly — only for apps where the
/// heuristic is known to fail:
///
///   - Anti-cheat protected games (Vanguard, EAC, BattlEye) where
///     `check_d3d_vk_modules` is denied PROCESS_VM_READ, AND the cpu/threads
///     fallback may also be unreliable when the agent itself runs unprivileged
///     enough that NtQuerySystemInformation returns coarse data.
///   - Frame-gen / upscaler companion apps (Lossless Scaling) that do heavy
///     per-frame ML work but are not themselves the foreground fullscreen
///     window — they run alongside the game they're scaling, so the
///     is_fullscreen gate and the auto-discovery foreground probe both miss
///     them.
inline constexpr const char* kGameNames[] = {
    // ── Anti-cheat protected (Vanguard) — Riot ──────────────────────────
    "League of Legends.exe",   // LoL game process (NOT LeagueClient*)
    // ── Frame-gen / upscaler companions ─────────────────────────────────
    "LosslessScaling.exe",     // ML upscaler + frame-gen overlay
    nullptr,
};

/// Game storefront launchers and their internal helper processes.
/// These often use Chromium Embedded Framework (CEF) which loads d3d11.dll
/// and can briefly run fullscreen / busy, fooling the Game heuristic.
/// Treated as Productivity (observed, never auto-pinned).
inline constexpr const char* kLauncherHelperNames[] = {
    // Steam
    "steam.exe",
    "steamwebhelper.exe",      // CEF UI renderer — main culprit for false-Game
    "steamservice.exe",
    // Epic Games
    "EpicGamesLauncher.exe",
    "EpicWebHelper.exe",
    // GOG / Galaxy
    "GalaxyClient.exe",
    "GalaxyClientService.exe",
    "GalaxyCommunication.exe",
    "GalaxyOverlay.exe",
    // EA Desktop / Origin
    "EADesktop.exe",
    "EALauncher.exe",
    "Origin.exe",
    "OriginWebHelperService.exe",
    "EABackgroundService.exe",
    // Ubisoft Connect
    "UbisoftConnect.exe",
    "upc.exe",
    "UplayWebCore.exe",
    // Battle.net
    "Battle.net.exe",
    "Agent.exe",
    // Riot
    "RiotClientServices.exe",
    "RiotClientUx.exe",
    "RiotClientUxRender.exe",
    // League of Legends launcher chain — CEF-based; the actual game
    // (League of Legends.exe) is matched separately by kGameNames.
    "LeagueClient.exe",
    "LeagueClientUx.exe",
    "LeagueClientUxRender.exe",
    // Rockstar
    "Launcher.exe",
    "LauncherPatcher.exe",
    "RockstarService.exe",
    // Xbox PC / Microsoft Store
    "XboxPcApp.exe",
    "GameBar.exe",
    "GameBarFTServer.exe",
    nullptr,
};

/// Well-known Windows OS processes — never touched by Phynned.
inline constexpr const char* kSystemNames[] = {
    "System",         "Registry",        "smss.exe",
    "csrss.exe",      "wininit.exe",     "winlogon.exe",
    "services.exe",   "lsass.exe",       "svchost.exe",
    "fontdrvhost.exe","dwm.exe",         "explorer.exe",
    "SearchIndexer.exe","WmiPrvSE.exe",  "spoolsv.exe",
    "MsMpEng.exe",    "SecurityHealth.exe",
    nullptr,
};

// ── DLL-presence cache ──────────────────────────────────────────────────────
/// Per-PID cache for EnumProcessModules result (refreshed if PID re-appears).
struct DllCacheEntry {
    uint32_t pid;
    bool     has_d3d_or_vk;
    bool     checked;          // false → not yet queried
    uint8_t  _pad[2];
};
static constexpr uint32_t kDllCacheMax = 64u;

// ── ProcessClassifier ────────────────────────────────────────────────────────
/// Stateful classifier: caches DLL presence per PID.
/// Call classify() once per target per tick (ClassificationCache adds TTL on top).
///
/// Extends `phyriad::stigmergy::Classifier<ProcessInfo,
/// TargetKind>`, exposing Phynned's classification as an instance of Phyriad's
/// stigmergic dispatch pattern.
///
/// `final` allows LTO to devirtualize calls through the virtual `decide()`
/// when the concrete type is known at the call site (which is always the
/// case in Phynned — AgentRuntime owns the concrete ProcessClassifier).
/// The non-virtual `classify()` remains the hot-path entry point, preserved
/// for backwards-compatibility and zero-cost direct dispatch.
class ProcessClassifier final
    : public phyriad::stigmergy::Classifier<ProcessInfo, TargetKind>
{
public:
    ProcessClassifier() noexcept = default;

    ProcessClassifier(const ProcessClassifier&)            = delete;
    ProcessClassifier& operator=(const ProcessClassifier&) = delete;

    /// Classify a process. Returns Unknown when signals are insufficient.
    /// Hot-path entry point — non-virtual, direct call.
    [[nodiscard]] TargetKind classify(const ProcessInfo& p) noexcept;

    /// §P-0.5.3 — stigmergy::Classifier interface implementation.
    /// Forwards to classify() so the virtual override has zero divergence
    /// from the hot path. With `final` + LTO, the compiler can devirtualize
    /// when the concrete type is known.
    [[nodiscard]] TargetKind decide(const ProcessInfo& p) noexcept override {
        return classify(p);
    }

    /// Query whether the process has a graphics-API DLL loaded
    /// (D3D9/11/12, DXGI, Vulkan-1, OpenGL32, GLFW, LWJGL).
    /// Result is cached per PID; re-queries if PID not in cache.
    /// The name "d3d_vk" is kept for compatibility; also covers OpenGL
    /// to detect Java/LWJGL games such as Minecraft.
    [[nodiscard]] bool check_d3d_vk_modules(uint32_t pid) noexcept;

    /// Evict a PID from the DLL cache (call when target exits).
    void evict_pid(uint32_t pid) noexcept;

    /// Clear entire DLL cache.
    void clear_cache() noexcept;

    /// Attach a KindOverrides table. classify() consults it before any of
    /// its built-in lists or heuristics — so users can correct a
    /// misclassification (e.g. mark a CEF-based launcher helper as
    /// Productivity) without code changes. Passing nullptr disables the
    /// override path (default).
    void set_overrides(const class KindOverrides* o) noexcept { overrides_ = o; }

private:
    alignas(64) std::array<DllCacheEntry, kDllCacheMax> dll_cache_{};
    uint32_t dll_cache_n_{0u};
    const class KindOverrides* overrides_{nullptr};  // non-owning, may be null

    // ── Helpers ──────────────────────────────────────────────────────────────
    [[nodiscard]] static bool name_in_list(const char* exe,
                                           const char* const* list) noexcept;
    [[nodiscard]] static bool is_system_process(const ProcessInfo& p) noexcept;
    [[nodiscard]] static bool matches_known_stream(const char* exe) noexcept;
    [[nodiscard]] static bool matches_known_comm(const char* exe) noexcept;
    [[nodiscard]] static bool matches_known_browser(const char* exe) noexcept;
    [[nodiscard]] static bool matches_known_productivity(const char* exe) noexcept;
    [[nodiscard]] static bool matches_known_launcher_helper(const char* exe) noexcept;
    [[nodiscard]] static bool matches_known_game(const char* exe) noexcept;

    DllCacheEntry* find_dll_entry(uint32_t pid) noexcept;
};

} // namespace phynned::observer
// Made with my soul - Swately <3
