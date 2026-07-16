> **📦 Archived planning document — historical.**
> From Phyriad's pre-rebrand era (the project was codenamed *gamma* / *gma*).
> Published for transparency about how the project was actually built — **not**
> a current specification. Version numbers and performance claims here
> (including any "vs Folly" / "production-ready" framing) reflect the project's
> state at the time of writing and may since have been revised or **retracted**;
> the project has reset to `v0.1.0-experimental`. For current status see the
> repo `README.md` and `docs/planning/`. For benchmark-claim validation status
> see `docs/planning/TEST_INVENTORY.md`.

# Gamma Feature Requests from Ayama

*Ayama v0.1 · Filed against Gamma Framework 1.0.0*

This document tracks features that Ayama would benefit from but that currently
require workarounds. Items are ranked by impact (High/Med/Low) and effort.

> **All FR-1 through FR-7 are IMPLEMENTED** as of Gamma 1.0.0 Phase 4/FR-7
> (May 2026). See per-item implementation notes below.

---

## FR-1: `hw::topology()` free function

**Priority:** High
**Status:** ✅ IMPLEMENTED — `pillars/topology` (Phase 4, FR-1)

**Request:** Expose a `gma::hw::topology()` free function that returns
`const gma::HardwareTopology&` (singleton reference, probed once) so callers
don't need to call `HardwareTopology::probe()` and handle `std::expected`.

**Implementation (2026-05-14):**
- File: `pillars/topology/src/HardwareTopology.cpp`
- File: `pillars/topology/include/gamma/topology/HardwareTopology.hpp`
- Function-local static `TopologyCache` (C++11 thread-safe init) stores the
  probed topology and any probe error message.
- On probe failure returns a sentinel `HardwareTopology{}` (all-empty, safe to
  query with zero results).
- Companion function `gma::hw::last_probe_error() noexcept → std::string_view`
  returns the last probe diagnostic string.

**Migration from workaround:**
```cpp
// Before (workaround):
auto topo_r = gma::HardwareTopology::probe();
if (!topo_r) { /* handle error */ }
const auto& topo = *topo_r;

// After (FR-1):
const auto& topo = gma::hw::topology();
// Optionally check: gma::hw::last_probe_error().empty()
```

---

## FR-2: `HardwareTopology::ccd_count()` method

**Priority:** High
**Status:** ✅ IMPLEMENTED — `pillars/topology` (Phase 4, FR-2)

**Request:** Add `uint32_t ccd_count() const noexcept` to `HardwareTopology`
that returns the number of distinct CCD IDs in `cores`.

**Implementation (2026-05-14):**
- File: `pillars/topology/include/gamma/topology/HardwareTopology.hpp`
  — Added `uint32_t ccd_count_{0u}` private field; public accessor
    `[[nodiscard]] uint32_t ccd_count() const noexcept`.
- File: `pillars/topology/src/HardwareTopology.cpp`
  — Both Linux and Windows `probe()` bodies now compute
    `ccd_count_ = cores.empty() ? 0u : max(ccd_id)+1`.

**Migration from workaround:**
```cpp
// Before (workaround):
uint32_t max_ccd = 0u;
for (const auto& c : topo.cores)
    if (c.ccd_id > max_ccd) max_ccd = c.ccd_id;
const uint32_t n_ccd = topo.cores.empty() ? 0u : max_ccd + 1u;

// After (FR-2):
const uint32_t n_ccd = topo.ccd_count();
```

---

## FR-3: `hw::set_process_affinity(pid, mask)` — process-level affinity API

**Priority:** High
**Status:** ✅ IMPLEMENTED — `pillars/topology` (Phase 4, FR-3)

**Request:** Expose affinity get/set in the `hw::` namespace so Gamma tools and
Ayama share one implementation.

**Implementation (2026-05-14):**
- File: `pillars/topology/include/gamma/topology/HardwareTopology.hpp`
- File: `pillars/topology/src/HardwareTopology.cpp`

```cpp
namespace gma::hw {
    // Returns the PREVIOUS affinity mask on success.
    [[nodiscard]] std::expected<uint64_t, gma::Error>
    set_process_affinity(uint32_t pid, uint64_t mask) noexcept;

    [[nodiscard]] std::expected<uint64_t, gma::Error>
    get_process_affinity(uint32_t pid) noexcept;
}
```

- Windows: `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
  PROCESS_SET_INFORMATION)` + `GetProcessAffinityMask` /
  `SetProcessAffinityMask`.
- POSIX: `sched_getaffinity` / `sched_setaffinity` with `uint64_t ↔ cpu_set_t`
  conversion (up to 64 logical CPUs).
- `mask == 0` → `ErrorCode::InvalidArgument`.
- Privilege failure → `ErrorCode::PermissionDenied`.

**Migration from workaround:**
```cpp
// Before (workaround in Ayama):
HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
SetProcessAffinityMask(h, mask);
CloseHandle(h);

// After (FR-3):
auto r = gma::hw::set_process_affinity(pid, mask);
if (!r) { /* r.error().code == gma::ErrorCode::PermissionDenied */ }
```

---

## FR-4: `ProcessInfoProvider` — lightweight process enumeration

**Priority:** Med
**Status:** ✅ IMPLEMENTED — `pillars/process` (Phase 4, FR-4)

**Request:** A thin `gma::proc::enumerate_processes()` for fast-path per-tick
process enumeration without heavyweight metrics.

**Implementation (2026-05-14):**
- New pillar: `pillars/process/` (CMake target: `gamma_process`)
- Header: `pillars/process/include/gamma/process/ProcessEnumerator.hpp`
- Source: `pillars/process/src/ProcessEnumerator.cpp`

```cpp
namespace gma::proc {
    struct alignas(8) ProcessEntry {
        uint32_t pid;           // 4 B
        uint32_t parent_pid;    // 4 B
        uint64_t start_time;    // 8 B  (platform epoch, 0 if unavailable)
        char     name[64];      // 64 B (null-terminated exe base name)
    };  // sizeof == 80, trivially_copyable, standard_layout
    static_assert(sizeof(ProcessEntry) == 80u);

    inline constexpr uint32_t kMaxProcesses = 1024u;

    // Fills `out[0..return_value)` with up to `max_count` entries.
    // Zero-alloc: caller provides the buffer.
    [[nodiscard]] uint32_t
    enumerate_processes(ProcessEntry* out, uint32_t max_count) noexcept;

    // Last enumeration error diagnostic (thread-local, empty on success).
    [[nodiscard]] std::string_view last_enumerate_error() noexcept;
}
```

- Windows: `EnumProcesses` + `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` +
  `QueryFullProcessImageNameA` + `GetProcessTimes`.
- Linux: iterates `/proc/<pid>/comm` via `opendir`/`readdir`.
- Zero heap allocation — caller-provided buffer, thread-local PID scratch space.
- Links `psapi` on Windows.

**Migration from workaround:**
```cpp
// After (FR-4):
gma::proc::ProcessEntry entries[gma::proc::kMaxProcesses];
const uint32_t n = gma::proc::enumerate_processes(entries, std::size(entries));
for (uint32_t i = 0; i < n; ++i)
    process(entries[i]);
```

---

## FR-5: `gma::ipc::ShmRegion` — typed shared memory helper

**Priority:** Med
**Status:** ✅ IMPLEMENTED — `pillars/ipc` (Phase 4, FR-5)

**Request:** A `ShmRegion<Header, Payload>` template that handles platform-
neutral open/create/unlink, seqlock read/write guards, and connection detection.

**Implementation (2026-05-14):**
- New pillar: `pillars/ipc/` (CMake INTERFACE target: `gamma_ipc`)
- Header-only: `pillars/ipc/include/gamma/ipc/ShmRegion.hpp`

```cpp
namespace gma::ipc {
    // Concept enforced on Header:
    template <typename T>
    concept ShmHeaderConcept = requires(T& h) {
        { h.magic     } -> std::convertible_to<uint32_t&>;
        { h.version   } -> std::convertible_to<uint32_t&>;
        { h.seq       } -> std::convertible_to<std::atomic<uint32_t>&>;
        { h.agent_pid } -> std::convertible_to<std::atomic<uint32_t>&>;
    };

    template <ShmHeaderConcept Header, typename Payload>
    class ShmRegion {
    public:
        [[nodiscard]] static std::expected<ShmRegion, gma::Error>
        create(const char* name, uint32_t magic,
               uint32_t version = 1u) noexcept;

        [[nodiscard]] static std::expected<ShmRegion, gma::Error>
        open(const char* name, uint32_t magic,
             uint32_t expected_version = 1u) noexcept;

        // RAII write guard: seq odd→even on construction/destruction.
        [[nodiscard]] auto begin_write() noexcept;

        // Seqlock read — retries up to 3× for consistency.
        [[nodiscard]] bool
        try_read_consistent(Payload* out) const noexcept;

        void close() noexcept;
    };
}
```

- Windows: `CreateFileMappingW` / `MapViewOfFile` / `UnmapViewOfFile` /
  `CloseHandle`.
- POSIX: `shm_open` / `ftruncate` / `mmap` / `munmap` / `shm_unlink` (owner
  only).
- Magic written LAST after full payload init with `atomic_thread_fence(release)`.
- Header-only INTERFACE library (no `.cpp`); no system link deps on Windows,
  optional `-lrt` on older Linux.

---

## FR-6: `gma::etw::SessionManager` — ETW session lifecycle

**Priority:** Med
**Status:** ✅ IMPLEMENTED — `pillars/etw` (Phase 4, FR-6)

**Request:** If Gamma gains an ETW abstraction layer (Windows performance
monitoring), Ayama would prefer it over maintaining its own raw-ETW code.

**Implementation (2026-05-14):**
- New pillar: `pillars/etw/` (CMake target: `gamma_etw`)
- Header: `pillars/etw/include/gamma/etw/SessionManager.hpp`
- Source: `pillars/etw/src/SessionManager.cpp`

```cpp
namespace gma::etw {
    struct ProviderSpec {
        GUID guid; UCHAR level; ULONGLONG match_any; ULONGLONG match_all;
    };
    using EventCallback =
        void (*)(const EVENT_RECORD& rec, void* user_ctx) noexcept;

    class SessionManager {
    public:
        [[nodiscard]] std::expected<void, gma::Error>
        start(const char* session_name,
              std::span<const ProviderSpec> providers,
              uint32_t buffer_size_kb = 64u,
              uint32_t max_buffers    = 32u) noexcept;

        void stop() noexcept;

        [[nodiscard]] std::expected<void, gma::Error>
        start_consumer(EventCallback cb, void* user_ctx) noexcept;

        void stop_consumer() noexcept;

        [[nodiscard]] uint64_t events_processed() const noexcept;
    };

    namespace providers {
        inline constexpr GUID kKernelProcess{...};      // Microsoft-Windows-Kernel-Process
        inline constexpr GUID kKernelThread{...};       // Microsoft-Windows-Kernel-Thread
        inline constexpr GUID kKernelContextSwitch{...};// Microsoft-Windows-Kernel-Dispatcher
        inline constexpr GUID kKernelMemory{...};       // Microsoft-Windows-Kernel-Memory
    }
}
```

- Windows: `StartTraceA` + `EnableTraceEx2` + `OpenTraceW` + `ProcessTrace`
  (background thread) + `ControlTrace STOP`.
- Non-Windows: header-only no-op stubs; `start()` returns `Unavailable`.
- Existing sessions with same name are stopped first (idempotent restart).
- Admin required on Windows 10+ — returns `PermissionDenied` if denied.
- Links `advapi32` + `tdh` on Windows.

**Migration from workaround:**
```cpp
// After (FR-6):
gma::etw::SessionManager session;
gma::etw::ProviderSpec providers[] = {
    {gma::etw::providers::kKernelProcess, TRACE_LEVEL_INFORMATION, 0x10u, 0u}
};
auto r = session.start("AyamaSession", providers);
if (!r && r.error().code != gma::ErrorCode::PermissionDenied) { /* fatal */ }
session.start_consumer(my_callback, &my_ctx);
// ...agent tick loop...
session.stop();
```

---

## FR-7: Node hot-restart support

**Priority:** Low
**Status:** ✅ IMPLEMENTED — `pillars/runtime` (Phase 4, FR-7)

**Request:** Allow individual nodes in a graph to be stopped and restarted
without tearing down the entire graph.

**Implementation (2026-05-14):**
Option B ("soft pause/resume") from the implementation strategies was chosen as
the most pragmatic approach. Full node replacement (Option C) was deferred.

- File: `pillars/runtime/include/gamma/runtime/GraphRuntime.hpp`
- File: `pillars/runtime/src/GraphRuntime.cpp`

```cpp
namespace gma::runtime {
    class GraphRuntime {
    public:
        // Pause: tick() is skipped by the run loop until resume_node().
        // Thread-safe — safe to call while run() is active.
        [[nodiscard]] std::expected<void, Error>
        pause_node(NodeId id) noexcept;

        // Resume a paused node. Idempotent.
        [[nodiscard]] std::expected<void, Error>
        resume_node(NodeId id) noexcept;

        // Soft-restart: pause → on_stop() → on_start() → resume.
        // The run loop will not tick this node during the sequence.
        [[nodiscard]] std::expected<void, Error>
        restart_node(NodeId id) noexcept;

        // Returns true if the node is currently paused.
        [[nodiscard]] bool is_node_paused(NodeId id) const noexcept;
    };
}
```

**Internals:** `GraphRuntime::Impl` holds
`std::unique_ptr<std::atomic<bool>[]> paused_` (one flag per node, sized
at `create()` time). The `run()` loop checks
`paused_[nid].load(std::memory_order_acquire)` before calling `tick()`.
All four methods return `ErrorCode::InvalidNodeId` for out-of-range IDs.

**Note:** `ErrorCode::NodePaused = 4` was already reserved in
`pillars/schema/include/gamma/schema/Error.hpp` — confirming FR-7 was
anticipated in the original schema design.

**Migration for Ayama ETW dynamic stop/start pattern:**
```cpp
// Stop ETW-sourcing node temporarily without graph teardown:
auto r = runtime.pause_node(etw_source_node_id);
// ... reconfigure ETW session ...
(void)runtime.resume_node(etw_source_node_id);

// Or full soft-restart (on_stop + on_start + resume):
(void)runtime.restart_node(etw_source_node_id);
```

---

## FR-8: Extended `gma::ErrorCode` values

**Priority:** High
**Status:** 📋 REQUESTED — filed 2026-05-14 for Ayama Phase 1 unblocking

**Request:** The following `ErrorCode` values are used by Ayama but are absent
from `pillars/schema/include/gamma/schema/Error.hpp`. Without them Ayama fails
to compile.

| Value name      | Requested = | Use site(s)                                              |
|-----------------|-------------|----------------------------------------------------------|
| `SystemError`   | 23          | `ActionExecutor` (GetProcessAffinityMask fails), `AyamaAgentPublisher` (Win32 SHM APIs) |
| `BufferFull`    | 24          | `ActionExecutor::apply()` (active-action table full)     |
| `OutOfMemory`   | 25          | `AgentRuntime::start()` (`new (std::nothrow)` returns null) |

**Proposed addition to `pillars/schema/include/gamma/schema/Error.hpp`:**
```cpp
// ── Phase 5 / Ayama additions ─────────────────────────────────────────────
SystemError       = 23,  // OS/Win32/POSIX call returned an unexpected error
BufferFull        = 24,  // fixed-capacity buffer is at capacity
OutOfMemory       = 25,  // operator new / malloc returned null
```

---

## FR-9: `gma::hw::set_process_priority()` / `get_process_priority()`

**Priority:** High
**Status:** 📋 REQUESTED — filed 2026-05-14 for Ayama Phase 1 unblocking

**Request:** Ayama's `ActionExecutor` needs to read and change the priority
class of external processes — exactly as FR-3 did for affinity masks. The
current `gma::daemon::CrossProcessAffinity::set_priority()` is an instance
method; Ayama needs a zero-overhead free function in the same `gma::hw::` style
as FR-3's `set_process_affinity()`.

**Proposed API (extends `pillars/topology/`, same file as FR-3):**
```cpp
namespace gma::hw {
    /// Set the priority class of process `pid`.
    /// Windows: priority_class = NORMAL_PRIORITY_CLASS (0x20),
    ///          HIGH_PRIORITY_CLASS (0x80), ABOVE_NORMAL_PRIORITY_CLASS (0x8000), etc.
    /// POSIX:   priority_class is treated as a nice value (−20..19).
    ///
    /// Returns the PREVIOUS priority class on success so the caller can revert.
    /// Returns PermissionDenied if the process cannot be opened for
    /// PROCESS_SET_INFORMATION (Windows) or CAP_SYS_NICE (Linux).
    /// Returns InvalidArgument if priority_class == 0 (Windows sentinel for
    /// "GetPriorityClass failed").
    [[nodiscard]] std::expected<uint32_t, gma::Error>
    set_process_priority(uint32_t pid, uint32_t priority_class) noexcept;

    /// Read the current priority class without modifying it.
    [[nodiscard]] std::expected<uint32_t, gma::Error>
    get_process_priority(uint32_t pid) noexcept;
}
```

**Migration for ActionExecutor:**
```cpp
// Before (broken static call):
auto r = gma::daemon::CrossProcessAffinity::set_priority(pid, pclass);

// After (FR-9):
auto r = gma::hw::set_process_priority(pid, pclass);
if (!r) return std::unexpected(r.error());
out_prev_pclass = *r;
```

---

## FR-10: `gma::tuning::check_privilege_level()` free function

**Priority:** High
**Status:** 📋 REQUESTED — filed 2026-05-14 for Ayama Phase 1 unblocking

**Request:** `AgentRuntime` calls `gma::tuning::check_privilege_level()` to
obtain the effective `PrivilegeLevel` of the current process. The existing API
is `PrivilegeCheck::probe()` (a static method returning `PrivilegeInfo`).
Ayama only needs the `level` field, and expects a free function — not a class
static.

**Proposed addition to `pillars/tuning/include/gamma/tuning/PrivilegeCheck.hpp`:**
```cpp
namespace gma::tuning {
    /// Convenience free function: returns the privilege level of this process.
    /// Equivalent to PrivilegeCheck::probe().level.
    [[nodiscard]] inline PrivilegeLevel check_privilege_level() noexcept {
        return PrivilegeCheck::probe().level;
    }
}
```

This is a header-only one-liner — no new source file or CMake change needed.

---

## Implementation notes for Gamma team

When implementing any of these, please:

1. Keep all new APIs `noexcept` to match Ayama's strict `noexcept` boundary.
2. Avoid heap allocation in singleton paths (pre-allocated, cached).
3. Use `std::expected<T, gma::Error>` for fallible APIs (consistent with existing).
4. Test on both Windows 10 22H2 and Windows 11 — Ayama's primary platform.

---

*Last updated: 2026-05-14 — FR-1 through FR-7 implemented; FR-8/9/10 filed for Ayama Phase 1*
*Implementation by: Gamma team (Phase 4/5)*
