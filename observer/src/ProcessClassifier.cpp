// observer/src/ProcessClassifier.cpp
// ProcessClassifier — implementation.
//

#include <phynned/observer/ProcessClassifier.hpp>
#include <phynned/observer/KindOverrides.hpp>  // for ::lookup() in classify()

#include <cstring>
#include <cctype>
#include <cstdio>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
#endif

namespace phynned::observer {

// ── Case-insensitive name comparison ─────────────────────────────────────────
static bool istr_eq(const char* a, const char* b) noexcept
{
#ifdef _WIN32
    return ::_stricmp(a, b) == 0;
#else
    while (*a && *b) {
        if (std::tolower(static_cast<unsigned char>(*a))
         != std::tolower(static_cast<unsigned char>(*b)))
            return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
#endif
}

// ── Static helpers ─────────────────────────────────────────────────────────
/*static*/ bool ProcessClassifier::name_in_list(const char* exe,
                                                 const char* const* list) noexcept
{
    for (const char* const* p = list; *p != nullptr; ++p) {
        if (istr_eq(exe, *p)) return true;
    }
    return false;
}

/*static*/ bool ProcessClassifier::is_system_process(const ProcessInfo& p) noexcept
{
    // PID 0 (idle), 4 (System) — always system.
    if (p.pid == 0u || p.pid == 4u) return true;
    return name_in_list(p.exe_name, kSystemNames);
}

/*static*/ bool ProcessClassifier::matches_known_stream(const char* exe) noexcept
{
    return name_in_list(exe, kStreamNames);
}

/*static*/ bool ProcessClassifier::matches_known_comm(const char* exe) noexcept
{
    return name_in_list(exe, kCommNames);
}

/*static*/ bool ProcessClassifier::matches_known_browser(const char* exe) noexcept
{
    return name_in_list(exe, kBrowserNames);
}

/*static*/ bool ProcessClassifier::matches_known_productivity(const char* exe) noexcept
{
    return name_in_list(exe, kProductivityNames);
}

/*static*/ bool ProcessClassifier::matches_known_launcher_helper(const char* exe) noexcept
{
    return name_in_list(exe, kLauncherHelperNames);
}

/*static*/ bool ProcessClassifier::matches_known_game(const char* exe) noexcept
{
    return name_in_list(exe, kGameNames);
}

// ── DLL cache management ──────────────────────────────────────────────────
DllCacheEntry* ProcessClassifier::find_dll_entry(uint32_t pid) noexcept
{
    for (uint32_t i = 0u; i < dll_cache_n_; ++i) {
        if (dll_cache_[i].pid == pid) return &dll_cache_[i];
    }
    return nullptr;
}

void ProcessClassifier::evict_pid(uint32_t pid) noexcept
{
    for (uint32_t i = 0u; i < dll_cache_n_; ++i) {
        if (dll_cache_[i].pid == pid) {
            // Swap with last entry
            dll_cache_[i] = dll_cache_[dll_cache_n_ - 1u];
            --dll_cache_n_;
            return;
        }
    }
}

void ProcessClassifier::clear_cache() noexcept
{
    dll_cache_n_ = 0u;
}

bool ProcessClassifier::check_d3d_vk_modules(uint32_t pid) noexcept
{
    DllCacheEntry* entry = find_dll_entry(pid);
    if (entry != nullptr && entry->checked) {
        return entry->has_d3d_or_vk;
    }

    // Not cached yet — query via EnumProcessModulesEx (handles WoW64).
    bool found = false;

#ifdef _WIN32
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                              FALSE, static_cast<DWORD>(pid));
    if (proc != nullptr) {
        HMODULE mods[256];
        DWORD needed = 0u;
        // Mayo 2026 bug fix: use EnumProcessModulesEx with LIST_MODULES_ALL
        // instead of EnumProcessModules. The latter is documented to fail
        // when a 64-bit caller queries a 32-bit (WoW64) target — it returns
        // no modules at all, or wrong data. phynned-agent.exe is 64-bit, so
        // legacy 32-bit games (Borderlands 2, BioShock, Mass Effect original,
        // Skyrim LE, Fallout 3/NV, Half-Life 2/Source games, Civ V, GTA IV,
        // most pre-2015 games) were INVISIBLE to D3D detection — auto-
        // discovery wouldn't fire, classification stuck at Unknown, no
        // policy applied.
        //
        // LIST_MODULES_ALL covers both 32-bit and 64-bit modules in WoW64
        // processes, and is identical to EnumProcessModules for native procs.
        if (EnumProcessModulesEx(proc, mods, sizeof(mods), &needed,
                                  LIST_MODULES_ALL)) {
            const DWORD n = needed / sizeof(HMODULE);
            char mod_name[MAX_PATH];
            for (DWORD i = 0u; i < n && !found; ++i) {
                if (GetModuleBaseNameA(proc, mods[i], mod_name, sizeof(mod_name)) > 0u) {
                    // ── DirectX / Vulkan (mayoría de juegos AAA modernos) ────
                    if (istr_eq(mod_name, "d3d11.dll")    ||
                        istr_eq(mod_name, "d3d12.dll")    ||
                        istr_eq(mod_name, "d3d10.dll")    ||  // Source games (TF2, CS:S)
                        istr_eq(mod_name, "d3d9.dll")     ||  // BL2, BioShock, ME, Skyrim LE
                        istr_eq(mod_name, "d3d8.dll")     ||  // ancient games (GTA III/VC)
                        istr_eq(mod_name, "vulkan-1.dll") ||
                        istr_eq(mod_name, "dxgi.dll"))
                    {
                        found = true;
                    }
                    // ── OpenGL (Minecraft Java/LWJGL, Cemu, Yuzu, Slay the
                    // Spire, RimWorld, KSP, Factorio, Terraria, Stardew Valley)
                    // ────────────────────────────────────────────────────────
                    else if (istr_eq(mod_name, "opengl32.dll") ||
                             istr_eq(mod_name, "glfw.dll")     ||
                             istr_eq(mod_name, "glfw3.dll")    ||
                             // LWJGL-specific: nativos cargados por Java
                             istr_eq(mod_name, "lwjgl.dll")    ||
                             istr_eq(mod_name, "lwjgl_opengl.dll") ||
                             istr_eq(mod_name, "lwjgl_glfw.dll"))
                    {
                        found = true;
                    }
                }
            }
        }
        CloseHandle(proc);
    }
#endif
    // Insert or update cache entry.
    if (entry == nullptr) {
        if (dll_cache_n_ < kDllCacheMax) {
            entry = &dll_cache_[dll_cache_n_++];
        } else {
            // Cache full — evict entry 0 (oldest, simplistic LRU).
            entry = &dll_cache_[0];
        }
        entry->pid = pid;
    }
    entry->has_d3d_or_vk = found;
    entry->checked       = true;

    return found;
}

// ── classify() ───────────────────────────────────────────────────────────────
TargetKind ProcessClassifier::classify(const ProcessInfo& p) noexcept
{
    // 0. Manual override — UI-side editable, takes precedence over the
    //    classifier's own logic. Looked up by exe name only (not PID) so
    //    overrides survive PID recycling between game launches. Empty
    //    (no entries loaded) → no-op.
    if (overrides_) {
        TargetKind ok;
        if (overrides_->lookup(p.exe_name, ok)) return ok;
    }

    // 1. System guard — never touch.
    if (is_system_process(p)) return TargetKind::System;

    // 2. Exact name match — high confidence.
    if (matches_known_stream(p.exe_name))      return TargetKind::Stream;
    if (matches_known_comm(p.exe_name))        return TargetKind::Comm;
    if (matches_known_browser(p.exe_name))     return TargetKind::Browser;

    // 2b. Game-launcher helper processes (Steam, Epic, GOG, EA, Ubisoft,
    // Battle.net, Riot, Rockstar, Xbox). These often embed Chromium (CEF)
    // which loads d3d11.dll, and can briefly run fullscreen / sustain CPU
    // during user interaction with the storefront UI — meeting the Game
    // heuristic by accident. The most visible offender historically was
    // steamwebhelper.exe being pinned to V-Cache cores on X3D systems.
    //
    // Treat them as Productivity: observed, never auto-pinned, never
    // evicted. Returning here keeps them out of both the Game and
    // Productivity heuristic paths below.
    if (matches_known_launcher_helper(p.exe_name)) return TargetKind::Productivity;

    // 2c. Explicit Game names — bypass the heuristic gates.
    // Used for apps where the gate-based heuristic is known to fail:
    // anti-cheat protected games (Vanguard blocks PROCESS_VM_READ so
    // uses_d3d_or_vk stays false), and frame-gen / upscaler companions
    // (Lossless Scaling) that run alongside another fullscreen window
    // and therefore never pass the is_fullscreen gate themselves.
    if (matches_known_game(p.exe_name)) return TargetKind::Game;

    // 3. Game heuristics — gate-based model.
    //
    // Evolution:
    //   v1 (initial): 4 binary signals, threshold 3. Brittle.
    //   v2 (post-Fallout-4): added cpu/threads signals; flat-additive model.
    //       Problem: D3D + threads alone matched ANY CEF/Electron/Chromium
    //       app (Steam helpers, Discord, VSCode) when briefly foregrounded.
    //       SteamWebHelper was being misclassified as Game during user
    //       interaction with the Steam UI.
    //   v3 (current): gate-based. Fullscreen is REQUIRED — without it,
    //       no amount of CPU/thread/D3D activity classifies as Game.
    //       This matches reality: real games are always fullscreen (or
    //       borderless, which we now detect correctly post-ForegroundWatcher
    //       v2 with WS_CAPTION style check). Non-game apps with heavy
    //       graphics use (CEF, Electron) are NEVER fullscreen.
    //
    // Path 1 (fullscreen + minimal activity):
    //   The 99% common case. Real fullscreen + (graphics DLL OR busy work).
    //
    // Path 2 (windowed but sustained game-like behavior):
    //   For windowed games that ran long enough to obviously be games.
    //   Strict: must have ALL of D3D + cpu + threads + 60s+ foreground.
    //   Catches the rare user who plays in windowed mode.

    // ── Path 1: fullscreen + minimal additional evidence ──────────────────
    if (p.is_fullscreen && p.foreground_for_sec >= 5u) {
        const bool busy = (p.cpu_usage_pct > 15.0f) && (p.thread_count > 16u);
        if (p.uses_d3d_or_vk || busy) return TargetKind::Game;
    }

    // ── Path 2: windowed but sustained game-like behavior (rare) ──────────
    if (p.foreground_for_sec >= 60u
        && p.uses_d3d_or_vk
        && p.cpu_usage_pct > 15.0f
        && p.thread_count > 16u) {
        return TargetKind::Game;
    }

    // 4. Productivity — observe but no Auto action.
    if (matches_known_productivity(p.exe_name)) return TargetKind::Productivity;

    // 5. Unknown — no action in Auto mode.
    return TargetKind::Unknown;
}

} // namespace phynned::observer
// Made with my soul - Swately <3
