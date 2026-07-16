// framework/tuning/src/WindowsTuner.cpp
// WindowsTuner snapshot-aware implementation (power scheme / timer / working
// set / priority boost). All changes recorded in TuningSnapshot for crash-safe
//
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>      // CLSIDFromString
#include <powrprof.h>
#ifdef __GNUC__
#  include <mmsystem.h>
#else
#  include <timeapi.h>
#endif
// MSVC: link powrprof + winmm via #pragma comment.
// MinGW/Clang: linking is handled by CMake target_link_libraries (powrprof, winmm).
#ifdef _MSC_VER
#  pragma comment(lib, "powrprof.lib")
#  pragma comment(lib, "winmm.lib")
#endif

#include <phyriad/tuning/WindowsTuner.hpp>
#include <phyriad/tuning/PrivilegeCheck.hpp>
#include <phyriad/tuning/TuningProvider.hpp>
#include <phyriad/tuning/TuningSnapshot.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace phyriad::tuning {

// ── GUID helpers ─────────────────────────────────────────────────────────────

static void guid_to_str_(GUID const& g, char* buf, std::size_t len) noexcept {
    std::snprintf(buf, len,
        "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        static_cast<unsigned long>(g.Data1),
        g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static bool str_to_guid_(char const* s, GUID& out) noexcept {
    WCHAR wbuf[64]{};
    ::MultiByteToWideChar(CP_ACP, 0, s, -1, wbuf,
                          static_cast<int>(sizeof(wbuf) / sizeof(wbuf[0])));
    return ::CLSIDFromString(wbuf, &out) == S_OK;
}

// ── Per-feature apply helpers ────────────────────────────────────────────────

namespace {

std::expected<void, phyriad::Error>
apply_power_scheme_(TuningSnapshot& snap, bool dry_run) noexcept {
    // High Performance GUID — always present on Windows.
    static const GUID kHighPerf = {
        0x8c5e7fda, 0xe8bf, 0x4a96,
        {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};

    GUID* current_guid{};
    if (::PowerGetActiveScheme(nullptr, &current_guid) != ERROR_SUCCESS) {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }
    char old_str[64]{};
    guid_to_str_(*current_guid, old_str, sizeof(old_str));
    ::LocalFree(current_guid);

    char new_str[64]{};
    guid_to_str_(kHighPerf, new_str, sizeof(new_str));
    if (std::strcmp(old_str, new_str) == 0) return {};

    if (dry_run) {
        std::fprintf(stderr, "[dry-run] PowerScheme: '%s' -> '%s'\n",
                     old_str, new_str);
        return {};
    }

    if (::PowerSetActiveScheme(nullptr, &kHighPerf) != ERROR_SUCCESS) {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }
    snap.add_record(TuningOp::WinPowercfg, "PowerScheme", old_str, new_str);
    return {};
}

std::expected<void, phyriad::Error>
apply_timer_resolution_(TuningSnapshot& snap, bool dry_run) noexcept {
    static constexpr UINT kPeriodMs = 1u;
    TIMECAPS tc{};
    if (::timeGetDevCaps(&tc, sizeof(tc)) != MMSYSERR_NOERROR) {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }

    char old_str[16]{};
    std::snprintf(old_str, sizeof(old_str), "%u", tc.wPeriodMin);
    char new_str[16]{};
    std::snprintf(new_str, sizeof(new_str), "%u", kPeriodMs);

    if (tc.wPeriodMin == kPeriodMs) return {};

    if (dry_run) {
        std::fprintf(stderr, "[dry-run] TimerResolution: %u ms -> %u ms\n",
                     tc.wPeriodMin, kPeriodMs);
        return {};
    }

    if (::timeBeginPeriod(kPeriodMs) != TIMERR_NOERROR) {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }
    snap.add_record(TuningOp::WinTimer, "TimerResolution", old_str, new_str);
    return {};
}

std::expected<void, phyriad::Error>
apply_working_set_(TuningSnapshot& snap, bool dry_run) noexcept {
    // 512 MB minimum, 2 GB maximum — prevents paging of hot arenas.
    static constexpr SIZE_T kMinBytes = 512ull * 1024ull * 1024ull;
    static constexpr SIZE_T kMaxBytes = 2048ull * 1024ull * 1024ull;

    SIZE_T old_min{}, old_max{};
    if (!::GetProcessWorkingSetSize(::GetCurrentProcess(),
                                    &old_min, &old_max)) {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }

    char old_str[64]{};
    char new_str[64]{};
    std::snprintf(old_str, sizeof(old_str), "%zu:%zu", old_min, old_max);
    std::snprintf(new_str, sizeof(new_str), "%zu:%zu", kMinBytes, kMaxBytes);

    if (old_min == kMinBytes && old_max == kMaxBytes) return {};

    if (dry_run) {
        std::fprintf(stderr, "[dry-run] WorkingSet: '%s' -> '%s'\n",
                     old_str, new_str);
        return {};
    }

    if (!::SetProcessWorkingSetSize(::GetCurrentProcess(),
                                     kMinBytes, kMaxBytes)) {
        // Non-fatal: process may lack SeLockMemoryPrivilege.
        return {};
    }
    snap.add_record(TuningOp::WinWorkSet, "WorkingSet", old_str, new_str);
    return {};
}

std::expected<void, phyriad::Error>
disable_priority_boost_(TuningSnapshot& snap, bool dry_run) noexcept {
    BOOL is_disabled{};
    if (!::GetProcessPriorityBoost(::GetCurrentProcess(), &is_disabled)) {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }
    if (is_disabled == TRUE) return {};  // already disabled

    const char* old_str = "1";   // boost was enabled
    const char* new_str = "0";   // boost now disabled

    if (dry_run) {
        std::fprintf(stderr,
            "[dry-run] PriorityBoost: enabled -> disabled\n");
        return {};
    }

    // bDisablePriorityBoost = TRUE means "disable the dynamic boost".
    if (!::SetProcessPriorityBoost(::GetCurrentProcess(), TRUE)) {
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::IoError,
            .source_node_id = 0, .timestamp_ns = 0});
    }
    snap.add_record(TuningOp::WinPrioBoost, "PriorityBoost", old_str, new_str);
    return {};
}

} // anonymous namespace

// ── WindowsTuner ITuningProvider implementation ──────────────────────────────

std::expected<void, phyriad::Error>
WindowsTuner::apply_full(TuningSnapshot& snap,
                         TuningConfig const& cfg,
                         bool dry_run) noexcept
{
    const auto info = PrivilegeCheck::probe();
    (void)info;  // hint only — actual gating is per-op via API return values

    if (cfg.apply_power_scheme) {
        if (auto r = apply_power_scheme_(snap, dry_run); !r) return r;
    }
    if (cfg.apply_timer_resolution) {
        if (auto r = apply_timer_resolution_(snap, dry_run); !r) return r;
    }
    if (cfg.apply_working_set) {
        if (auto r = apply_working_set_(snap, dry_run); !r) return r;
    }
    if (cfg.disable_priority_boost) {
        if (auto r = disable_priority_boost_(snap, dry_run); !r) return r;
    }
    return {};
}

bool WindowsTuner::verify_snapshot(TuningSnapshot const& snap) const noexcept {
    for (auto const& r : snap.records()) {
        switch (r.op) {
        case TuningOp::WinPowercfg: {
            GUID* active{};
            if (::PowerGetActiveScheme(nullptr, &active) == ERROR_SUCCESS) {
                char cur[64]{};
                guid_to_str_(*active, cur, sizeof(cur));
                ::LocalFree(active);
                if (std::strcmp(cur, r.new_value) != 0) return false;
            }
            break;
        }
        case TuningOp::WinPrioBoost: {
            BOOL disabled{};
            if (::GetProcessPriorityBoost(::GetCurrentProcess(), &disabled)) {
                const char expected = disabled ? '0' : '1';
                if (r.new_value[0] != expected) return false;
            }
            break;
        }
        default: break;  // timer/workset are set-and-forget; no runtime read-back
        }
    }
    return true;
}

void WindowsTuner::reapply_snapshot(TuningSnapshot const& snap) noexcept {
    for (auto const& r : snap.records()) {
        switch (r.op) {
        case TuningOp::WinPowercfg: {
            GUID guid{};
            if (str_to_guid_(r.new_value, guid))
                ::PowerSetActiveScheme(nullptr, &guid);
            break;
        }
        case TuningOp::WinTimer: {
            const unsigned period =
                static_cast<unsigned>(std::atoi(r.new_value));
            if (period > 0) ::timeBeginPeriod(period);
            break;
        }
        case TuningOp::WinWorkSet: {
            std::size_t wmin = 0, wmax = 0;
            std::sscanf(r.new_value, "%zu:%zu", &wmin, &wmax);
            if (wmin > 0 || wmax > 0)
                ::SetProcessWorkingSetSize(::GetCurrentProcess(), wmin, wmax);
            break;
        }
        case TuningOp::WinPrioBoost: {
            const BOOL disable = (r.new_value[0] == '1') ? TRUE : FALSE;
            ::SetProcessPriorityBoost(::GetCurrentProcess(), disable);
            break;
        }
        default: break;
        }
    }
}

} // namespace phyriad::tuning

#endif // _WIN32
// Made with my soul - Swately <3
