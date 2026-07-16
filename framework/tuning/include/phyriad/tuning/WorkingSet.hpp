// framework/tuning/include/phyriad/tuning/WorkingSet.hpp
// Working-set size hints for the current process.
//
// Provides:
//   phyriad::tuning::set_self_working_set(min_bytes, max_bytes) — set RSS hint.
//   phyriad::tuning::get_self_working_set(out_current, out_peak) — query RSS.
//
// Semantics:
//   set_self_working_set is a HINT to the OS. The kernel may exceed the
//   requested limits under memory pressure. Use it to reduce paging for
//   performance-sensitive long-lived agents (real-time workers, monitoring
//   daemons, telemetry publishers).
//
//   Passing (0, 0) releases the hint, allowing the OS to manage freely.
//
// Platforms:
//   Windows: SetProcessWorkingSetSize / GetProcessMemoryInfo (PSAPI).
//            Requires SE_INC_BASE_PRIORITY_NAME for some SKUs; returns
//            PermissionDenied if the process lacks the right.
//   Linux:   setrlimit(RLIMIT_RSS) — soft limit only; modern kernels (>=4.x)
//            largely ignore it. Returns OK (best-effort). No-op in practice.
//   Other:   No-op; returns OK.
//
// Threading:
//   Safe to call from any thread. Typically called once during agent start().
//
#pragma once

#include <phyriad/schema/Error.hpp>
#include <cstdint>
#include <expected>
#include <utility>  // std::pair (GFR-Ayama-5)

namespace phyriad::tuning {

/// Hint the OS about desired working-set bounds for THIS process.
///
/// `min_bytes` — desired minimum RSS (bytes). 0 = OS default.
/// `max_bytes` — desired maximum RSS (bytes). 0 = OS default.
///
/// Returns OK on success (or accepted no-op on platforms that ignore the hint).
/// Returns PermissionDenied if the OS denies access (Windows privilege).
/// Returns InvalidArgument if min_bytes > max_bytes and both are non-zero.
[[nodiscard]] std::expected<void, phyriad::Error>
set_self_working_set(uint64_t min_bytes, uint64_t max_bytes) noexcept;

/// Query current and peak working-set bytes for THIS process.
///
/// `out_current` — filled with current RSS bytes (may be nullptr).
/// `out_peak`    — filled with peak RSS since start  (may be nullptr).
///
/// Returns OK on success. Values are approximate; kernel updates on page faults.
[[nodiscard]] std::expected<void, phyriad::Error>
get_self_working_set(uint64_t* out_current, uint64_t* out_peak) noexcept;

// ── GFR-Ayama-5: cross-process variant ──────────────────────────────────────
// Set working-set bounds for an ARBITRARY process by PID. Used to suggest
// min/max RSS for an externally-observed process — e.g. reducing
// streaming-event paging for an interactive workload while a separate
// observer process supervises it. See apps/ayama/ for a concrete consumer.
//
// Windows: OpenProcess(PROCESS_SET_QUOTA) + SetProcessWorkingSetSizeEx.
//          The non-Ex variant is hard-limit; the Ex variant with no flags
//          is a hint (recommended).  SeIncreaseQuotaPrivilege is required
//          for cross-process — present in admin context.
// Linux:   no portable cross-process equivalent (setrlimit is per-process
//          and ignored by modern kernels anyway). Returns Unavailable.
//
// `min_bytes` / `max_bytes` semantics match `set_self_working_set`. Pass
// (0, 0) to release any prior hint.
[[nodiscard]] std::expected<void, phyriad::Error>
set_process_working_set(uint32_t pid,
                        uint64_t min_bytes,
                        uint64_t max_bytes) noexcept;

/// Cross-process variant of `get_self_working_set` — returns the CURRENT
/// limit configuration rather than the actual RSS. Useful for verifying
/// that set_process_working_set took effect.
///
/// Returns the (min, max) pair that the kernel currently reports.
/// Windows: GetProcessWorkingSetSizeEx. Linux: Unavailable.
[[nodiscard]] std::expected<std::pair<uint64_t, uint64_t>, phyriad::Error>
get_process_working_set_limits(uint32_t pid) noexcept;

} // namespace phyriad::tuning
// Made with my soul - Swately <3
