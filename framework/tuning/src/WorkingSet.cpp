// framework/tuning/src/WorkingSet.cpp
// WorkingSet — implementation of set_self_working_set / get_self_working_set.
//

#include <phyriad/tuning/WorkingSet.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>    // GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS
#elif defined(__linux__)
#  include <sys/resource.h>
#  include <sys/types.h>
#  include <cstdio>
#  include <cstdlib>    // strtoul
#  include <cstring>
#  include <cerrno>
#endif

namespace phyriad::tuning {

// ── set_self_working_set ──────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
set_self_working_set(uint64_t min_bytes, uint64_t max_bytes) noexcept
{
    // Validate: if both non-zero, min must be ≤ max.
    if (min_bytes != 0u && max_bytes != 0u && min_bytes > max_bytes) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::InvalidArgument, 0u, 0u});
    }

#ifdef _WIN32
    const BOOL ok = SetProcessWorkingSetSize(
        GetCurrentProcess(),
        static_cast<SIZE_T>(min_bytes),
        static_cast<SIZE_T>(max_bytes));
    if (!ok) {
        const DWORD e = GetLastError();
        const phyriad::ErrorCode code =
            (e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD)
            ? phyriad::ErrorCode::PermissionDenied
            : phyriad::ErrorCode::SystemError;
        return std::unexpected(phyriad::Error{code, 0u, 0u});
    }
    return {};

#elif defined(__linux__)
    // setrlimit RLIMIT_RSS is a soft hint that modern kernels largely ignore,
    // but it is the correct POSIX mechanism. We treat failure as non-fatal.
    if (max_bytes == 0u) {
        // Release hint: set to unlimited.
        const struct rlimit r{RLIM_INFINITY, RLIM_INFINITY};
        (void)setrlimit(RLIMIT_RSS, &r);
    } else {
        const struct rlimit r{
            static_cast<rlim_t>(max_bytes),
            static_cast<rlim_t>(max_bytes)};
        (void)setrlimit(RLIMIT_RSS, &r);  // best-effort; don't propagate EPERM
    }
    return {};

#else
    // Other platforms: accepted no-op.
    (void)min_bytes;
    (void)max_bytes;
    return {};
#endif
}

// ── get_self_working_set ──────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
get_self_working_set(uint64_t* out_current, uint64_t* out_peak) noexcept
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError, 0u, 0u});
    }
    if (out_current) *out_current = static_cast<uint64_t>(pmc.WorkingSetSize);
    if (out_peak)    *out_peak    = static_cast<uint64_t>(pmc.PeakWorkingSetSize);
    return {};

#elif defined(__linux__)
    // Read VmRSS and VmPeak from /proc/self/status.
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::IoError, 0u, 0u});
    }
    char line[128]{};
    uint64_t rss_kb = 0u;
    uint64_t peak_kb = 0u;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            rss_kb = static_cast<uint64_t>(std::strtoul(line + 6, nullptr, 10));
        } else if (std::strncmp(line, "VmPeak:", 7) == 0) {
            peak_kb = static_cast<uint64_t>(std::strtoul(line + 7, nullptr, 10));
        }
    }
    std::fclose(f);
    if (out_current) *out_current = rss_kb  * 1024ull;
    if (out_peak)    *out_peak    = peak_kb * 1024ull;
    return {};

#else
    // Other: return zeros (no-op).
    if (out_current) *out_current = 0u;
    if (out_peak)    *out_peak    = 0u;
    return {};
#endif
}

// ── GFR-Ayama-5: cross-process working-set ──────────────────────────────────
std::expected<void, phyriad::Error>
set_process_working_set(uint32_t pid,
                        uint64_t min_bytes,
                        uint64_t max_bytes) noexcept
{
    // Same validation as the self variant.
    if (min_bytes != 0u && max_bytes != 0u && min_bytes > max_bytes) {
        return std::unexpected(
            phyriad::Error{phyriad::ErrorCode::InvalidArgument, 0u, 0u});
    }

#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_SET_QUOTA, FALSE,
                           static_cast<DWORD>(pid));
    if (!h) {
        const DWORD e = GetLastError();
        const phyriad::ErrorCode code =
            (e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD)
            ? phyriad::ErrorCode::PermissionDenied
            : phyriad::ErrorCode::InvalidArgument;
        return std::unexpected(phyriad::Error{code, 0u, 0u});
    }
    // Use the Ex variant with flags=0 → SOFT hint (kernel may exceed).
    // Hard limits would require QUOTA_LIMITS_HARDWS_* flags; soft is safer.
    const BOOL ok = SetProcessWorkingSetSizeEx(
        h,
        static_cast<SIZE_T>(min_bytes),
        static_cast<SIZE_T>(max_bytes),
        0u /* soft hint */);
    CloseHandle(h);
    if (!ok) {
        const DWORD e = GetLastError();
        const phyriad::ErrorCode code =
            (e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD)
            ? phyriad::ErrorCode::PermissionDenied
            : phyriad::ErrorCode::SystemError;
        return std::unexpected(phyriad::Error{code, 0u, 0u});
    }
    return {};

#else
    // No portable cross-process equivalent. Linux's setrlimit is per-process
    // and ignored anyway on modern kernels.
    (void)pid; (void)min_bytes; (void)max_bytes;
    return std::unexpected(phyriad::Error{phyriad::ErrorCode::Unavailable, 0u, 0u});
#endif
}

std::expected<std::pair<uint64_t, uint64_t>, phyriad::Error>
get_process_working_set_limits(uint32_t pid) noexcept
{
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
    if (!h) {
        const DWORD e = GetLastError();
        const phyriad::ErrorCode code =
            (e == ERROR_ACCESS_DENIED) ? phyriad::ErrorCode::PermissionDenied
                                       : phyriad::ErrorCode::InvalidArgument;
        return std::unexpected(phyriad::Error{code, 0u, 0u});
    }
    SIZE_T min_ws = 0u, max_ws = 0u;
    DWORD flags = 0u;
    const BOOL ok = GetProcessWorkingSetSizeEx(h, &min_ws, &max_ws, &flags);
    CloseHandle(h);
    if (!ok) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::SystemError, 0u, 0u});
    }
    return std::pair<uint64_t, uint64_t>{
        static_cast<uint64_t>(min_ws),
        static_cast<uint64_t>(max_ws)};

#else
    (void)pid;
    return std::unexpected(phyriad::Error{phyriad::ErrorCode::Unavailable, 0u, 0u});
#endif
}

} // namespace phyriad::tuning
// Made with my soul - Swately <3
