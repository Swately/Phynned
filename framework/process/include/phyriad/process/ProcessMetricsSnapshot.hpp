// framework/process/include/phyriad/process/ProcessMetricsSnapshot.hpp
// Bulk system-wide process metrics via a single OS call.
//
// Provides:
//   ProcessMetrics           — 64-byte POD for one process's CPU/memory/IO metrics.
//   ProcessMetricsSnapshot   — owns a buffer; call capture() each tick,
//                              then find(pid) or extract(pids, n, out).
//
// Performance rationale:
//   Per-PID sampling (OpenProcess + GetProcessTimes × n) costs ~3 µs/process.
//   For 32 targets × 3 metrics = 96 syscall pairs ≈ 300 µs/tick.
//
//   Bulk capture via NtQuerySystemInformation(SystemProcessInformation) on
//   Windows — one syscall for ALL processes — reduces this to:
//     ~80-150 µs for capture() (entire system snapshot)
//     ~600 ns for extract(32 pids)
//   Total: ~10× faster at scale, with ZERO steady-state allocations.
//
// API:
//   auto s = ProcessMetricsSnapshot::create();   // cold path
//   s->capture();                                // hot-path tick
//   const ProcessMetrics* m = s->find(pid);      // O(log N)
//   s->extract(pid_arr, n, out);                 // O(n + N) sorted-merge
//
// Windows implementation:
//   NtQuerySystemInformation(SystemProcessInformation) from ntdll.dll
//   (loaded via GetProcAddress — no import lib needed).
//   Buffer grows on STATUS_INFO_LENGTH_MISMATCH, stabilizes after 1-2 calls.
//
// Linux implementation:
//   Iterates /proc entries. O(N_proc) with per-PID /proc/<pid>/stat parsing.
//   Slower than Windows bulk API but correct and portable.
//
// Threading:
//   NOT thread-safe. Use one snapshot per thread, or protect externally.
//

#pragma once

#include <phyriad/schema/Error.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <type_traits>
#include <vector>

namespace phyriad::proc {

/// GFR-Ayama-3: per-thread snapshot data — POD, 48 bytes, alignas(8).
/// Fits 2 entries per 64-byte cache line; optimal for sequential scans
/// (e.g. scanning a target process's threads to locate the busiest one
/// for differential pinning — see apps/ayama/ for a concrete consumer).
///
/// All time fields are 100-nanosecond units since thread creation
/// (Windows FILETIME convention) on Windows; on Linux, derived from
/// /proc/<pid>/task/<tid>/stat fields (clock ticks).
///
/// `state` encoding (Windows kernel thread states):
///   0 = Initialized   3 = Standby
///   1 = Ready         4 = Terminated
///   2 = Running       5 = Waiting
///   6 = Transition    7 = Unknown
///
/// `priority` is KPRIORITY: -15..15 typical, 8 = NORMAL on Windows.
struct alignas(8) ThreadEntry {
    uint32_t tid;                  //  4B  @0
    uint32_t pid;                  //  4B  @4   owning process
    uint64_t kernel_time_100ns;    //  8B  @8
    uint64_t user_time_100ns;      //  8B  @16
    uint64_t create_time_100ns;    //  8B  @24
    int32_t  priority;             //  4B  @32  KPRIORITY (signed; -15..15)
    uint32_t state;                //  4B  @36  see encoding above
    uint64_t wait_reason;          //  8B  @40  if state == Waiting
};
static_assert(sizeof(ThreadEntry)  == 48u,
    "ThreadEntry must be exactly 48 bytes");
static_assert(alignof(ThreadEntry) == 8u,
    "ThreadEntry must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<ThreadEntry>,
    "ThreadEntry must be trivially copyable");
static_assert(std::is_standard_layout_v<ThreadEntry>,
    "ThreadEntry must be standard_layout");

/// Metrics for a single process — exactly one cache line (64 bytes).
struct alignas(8) ProcessMetrics {
    uint64_t kernel_time_100ns;  //  8B @0   accumulated kernel CPU time (100ns units)
    uint64_t user_time_100ns;    //  8B @8   accumulated user   CPU time (100ns units)
    uint64_t working_set_bytes;  //  8B @16  current resident set size (RSS)
    uint64_t private_bytes;      //  8B @24  committed private memory
    uint64_t read_bytes;         //  8B @32  I/O read bytes since process start
    uint64_t write_bytes;        //  8B @40  I/O write bytes since process start
    uint32_t pid;                //  4B @48
    uint32_t thread_count;       //  4B @52
    uint32_t handle_count;       //  4B @56  (Windows handles; 0 on Linux)
    uint32_t _pad;               //  4B @60
};
static_assert(sizeof(ProcessMetrics) == 64u,
    "ProcessMetrics must be exactly 64 bytes (one cache line)");
static_assert(std::is_trivially_copyable_v<ProcessMetrics>,
    "ProcessMetrics must be trivially_copyable");
static_assert(std::is_standard_layout_v<ProcessMetrics>,
    "ProcessMetrics must be standard_layout");

class ProcessMetricsSnapshot {
public:
    /// Default initial buffer = 256 KB ≈ enough for ~500 processes on Win10/11.
    static constexpr uint32_t kDefaultBufferBytes = 262144u;

    ~ProcessMetricsSnapshot() noexcept;

    // Non-copyable (owns large buffer), movable.
    ProcessMetricsSnapshot(const ProcessMetricsSnapshot&)            = delete;
    ProcessMetricsSnapshot& operator=(const ProcessMetricsSnapshot&) = delete;
    ProcessMetricsSnapshot(ProcessMetricsSnapshot&&) noexcept;
    ProcessMetricsSnapshot& operator=(ProcessMetricsSnapshot&&) noexcept;

    /// Factory. Allocates the internal buffer (cold path).
    /// `initial_capacity_bytes` — pre-allocated OS-query buffer size.
    [[nodiscard]] static std::expected<ProcessMetricsSnapshot, phyriad::Error>
    create(uint32_t initial_capacity_bytes = kDefaultBufferBytes) noexcept;

    /// Capture system-wide metrics. Called once per tick.
    ///
    /// After first stabilisation (typically 1-2 calls), performs ZERO heap
    /// allocations. The internal buffer grows geometrically on SIZE_MISMATCH
    /// and stays at the new size for all subsequent captures.
    ///
    /// ~80-150 µs on Windows (NtQuerySystemInformation).
    /// ~2-5 ms on Linux (/proc walk, N_proc ≤ 200 typical).
    [[nodiscard]] std::expected<void, phyriad::Error> capture() noexcept;

    /// Find metrics for a single PID. O(log N) binary search.
    /// Returns nullptr if the PID is not in the last snapshot.
    [[nodiscard]] const ProcessMetrics* find(uint32_t pid) const noexcept;

    /// Bulk extract: for each PID in `pids[0..n)`, find its metrics and
    /// write the result into `out[i]` (index-aligned with pids[i]).
    ///
    /// If a PID is not found, `out[i]` has pid == pids[i] and all metric
    /// fields set to 0. Returns the count of PIDs actually matched.
    ///
    /// O(n log N) using individual binary searches — fast for n ≤ 64.
    [[nodiscard]] uint32_t extract(const uint32_t* pids, uint32_t n,
                                    ProcessMetrics* out) const noexcept;

    /// Number of processes in the last capture.
    [[nodiscard]] uint32_t process_count() const noexcept;

    /// Raw sorted array (sorted by PID ascending). Valid until next capture().
    [[nodiscard]] const ProcessMetrics* data() const noexcept;

    // ── GFR-Ayama-3: per-thread enumeration ─────────────────────────────────
    // capture() walks the OS process table once and already pulls the
    // SYSTEM_THREAD_INFORMATION arrays into its internal buffer. These
    // accessors expose that data with zero additional syscall.

    /// Number of threads the given PID has in the most recent snapshot.
    /// O(log N) using the existing PID-sorted index. Returns 0 if the PID
    /// is not in the last capture.
    [[nodiscard]] uint32_t
    thread_count_for(uint32_t pid) const noexcept;

    /// Bulk extract: writes up to `max_count` ThreadEntry rows for the
    /// given PID into `out`. Returns the count actually written
    /// (≤ thread_count_for(pid)).
    ///
    /// Cost: O(log N + threads_for_pid). NO syscall — data was captured
    /// by capture(). Typical: ~5 µs for 50 threads on a busy game process.
    ///
    /// On Linux this requires capture() to have walked /proc/<pid>/task
    /// (added by the GFR-Ayama-3 patch); pre-patch snapshots return 0.
    [[nodiscard]] uint32_t
    extract_threads(uint32_t pid,
                    ThreadEntry* out,
                    uint32_t max_count) const noexcept;

private:
    ProcessMetricsSnapshot() noexcept = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace phyriad::proc
// Made with my soul - Swately <3
