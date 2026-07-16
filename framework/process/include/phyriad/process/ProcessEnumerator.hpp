// framework/process/include/phyriad/process/ProcessEnumerator.hpp
// Lightweight, zero-heap-allocation process enumeration — cross-platform.
//
// Public API (phyriad::proc::):
//   ProcessEntry          — POD descriptor (80 bytes, alignas(8))
//   kMaxProcesses         — soft upper bound on concurrent processes
//   enumerate_processes() — fill caller-provided buffer; returns count written
//   last_enumerate_error()— error string from last failed call (empty = ok)
//
// Design:
//   The API uses a caller-allocated buffer (NOT std::vector) to guarantee
//   zero heap allocation on the hot path. Callers typically declare a
//   thread_local or stack array of kMaxProcesses entries.
//
//   Windows implementation: EnumProcesses + OpenProcess per PID.
//   Linux implementation:   readdir(/proc) + read(comm).
//
// Threading:
//   enumerate_processes() is thread-safe (no shared mutable state — uses a
//   thread_local PID buffer on Windows for intermediate storage).
//   last_enumerate_error() is NOT thread-safe across concurrent callers.
//
// Performance (approximate):
//   200 processes — Windows ~800 µs, Linux ~200 µs.
//   Frequency: designed to be called every ~100 ms from a monitoring loop.
//

#pragma once
#include <phyriad/schema/Error.hpp>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace phyriad::proc {

// ── ProcessEntry ─────────────────────────────────────────────────────────────
// 80 bytes. Aligns to 8 bytes. Trivially copyable — safe for memcpy / ring slots.
struct alignas(8) ProcessEntry {
    uint32_t pid;           //  4B — OS process ID
    uint32_t parent_pid;    //  4B — parent PID (0 if unavailable/expensive)
    uint64_t start_time;    //  8B — creation time (FILETIME epoch on Win,
                            //       jiffies-from-boot on Linux; 0 if unavailable)
    char     name[64];      // 64B — null-terminated exe short name (ASCII)
};
static_assert(sizeof(ProcessEntry)  == 80u,
    "ProcessEntry must be exactly 80 bytes");
static_assert(alignof(ProcessEntry) == 8u,
    "ProcessEntry must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<ProcessEntry>,
    "ProcessEntry must be trivially copyable");
static_assert(std::is_standard_layout_v<ProcessEntry>,
    "ProcessEntry must be standard-layout");

/// Soft upper bound on the number of processes to enumerate.
/// Callers should size their buffer to at least this value.
inline constexpr uint32_t kMaxProcesses = 1024u;

// ── enumerate_processes ───────────────────────────────────────────────────────
/// Fill `out[0..max_count)` with currently running processes.
///
/// Returns the number of entries written (always ≤ max_count).
/// On error: returns 0; call last_enumerate_error() for diagnostics.
///
/// `out` must point to at least `max_count` valid ProcessEntry slots.
/// `max_count == 0` is a no-op and returns 0.
///
/// No heap allocation. Uses thread_local storage for intermediate OS buffers.
[[nodiscard]] uint32_t
enumerate_processes(ProcessEntry* out, uint32_t max_count) noexcept;

/// Returns a diagnostic string from the most recent failed enumerate_processes()
/// call on this thread. Returns empty string_view when the last call succeeded.
[[nodiscard]] std::string_view last_enumerate_error() noexcept;

} // namespace phyriad::proc
// Made with my soul - Swately <3
