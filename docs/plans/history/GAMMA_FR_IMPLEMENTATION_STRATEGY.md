> **📦 Archived planning document — historical.**
> From Phyriad's pre-rebrand era (the project was codenamed *gamma* / *gma*).
> Published for transparency about how the project was actually built — **not**
> a current specification. Version numbers and performance claims here
> (including any "vs Folly" / "production-ready" framing) reflect the project's
> state at the time of writing and may since have been revised or **retracted**;
> the project has reset to `v0.1.0-experimental`. For current status see the
> repo `README.md` and `docs/planning/`. For benchmark-claim validation status
> see `docs/planning/TEST_INVENTORY.md`.

# Gamma Feature Request Implementation Strategy

*Análisis profundo y estrategia de implementación para las 4 FRs (GFR-Ayama-2..5)
+ ImPlot integration. Compliant con estándares de Gamma 1.0 (`zero-heap-on-hot`,
`noexcept`, `std::expected`, POD-IPC, static polymorphism where it pays).*

**Version**: 1.0 — 2026-05-17
**Pre-requisito de codear**: este documento debe ser revisado y aprobado.

---

## §0 — Principios rectores

Antes de tocar código de Gamma, fijamos los principios que **todas** las
FRs deben respetar. Vienen de `AYAMA_MASTER_PLAN.md §0.4` + observación
directa de los pillars actuales (`topology`, `process`, `tuning`).

### §0.1 Gamma's API contract (no se rompe)

| Regla | Justificación |
|---|---|
| **`noexcept` siempre en hot path** | Excepciones obligan a unwinding tables que matan el inlining + sumar ciclos al return path |
| **`std::expected<T, gma::Error>`** para fallible | Sin overhead vs raw pointer/sentinel pattern, mantiene type-safety, ya estándar en Gamma |
| **`[[nodiscard]]`** para todo lo que devuelve valor importante | Compiler-enforced contract — el caller NO puede ignorar fallos |
| **POD para IPC / buffer crossing** | `trivially_copyable + standard_layout + sizeof ≤ 4096`. Permite memcpy directo en SHM. |
| **Zero heap en steady state** | Caller provee buffers. Los singletons cachean en first-call y nunca re-allocan. |
| **`alignas(8)` mínimo, `alignas(64)` para cache-line crítico** | Evita false sharing en multi-thread, optimiza access patterns |
| **`static_assert(sizeof(X) == N)`** para layouts IPC | Layout drift detection en build time, no en runtime |
| **Cross-platform via `#ifdef`** | Win32 path + POSIX path; donde POSIX no aplique, `ErrorCode::Unavailable` stub |

### §0.2 Donde **SÍ paga** static polymorphism

Static polymorphism (templates, CRTP, concepts) tiene costo: complica
debugging, infla binarios, aumenta compile time. **Solo lo usamos donde
gana ciclos medibles o type-safety irremplazable.**

| Patrón | Cuándo usarlo | Cuándo NO |
|---|---|---|
| **Templates con concepts** | Type-generic containers, schema-validated payloads (Gamma usa esto en `ipc::Ring<T,N>`) | Wrappers thin de Win32 — el overhead virtual no existe ahí |
| **CRTP** | Policy hierarchies con dispatch hot-path (ej: Ayama's PolicyEngine podría) | Single-implementation paths |
| **`if constexpr`** | Compile-time branch elimination cuando el caller conoce un flag estático | Runtime branches (use plain `if`) |
| **`consteval` / `constexpr`** | Compile-time-computed tables (Gamma usa esto en hash provider GUIDs) | Anywhere with runtime input |

**Realidad de las 4 FRs**: son wrappers thin sobre Win32 syscalls. El
costo total está dominado por el syscall (`2-5 µs` cada uno). El call
overhead C++ es nanosegundos. **Templating estas FRs no ganaría ciclos
medibles** — solo añadiría complejidad. **Conclusión: NO templating.**

### §0.3 Donde **SÍ está la oportunidad de optimización real**

| Estrategia | Magnitud | FR aplicable |
|---|---|---|
| **Compartir syscall NtQSI** entre ProcessMetricsSnapshot y enumerate_threads | -50-150 µs por refresh tick (elimina un syscall completo) | **GFR-Ayama-3** (huge) |
| **Index PID→offset O(log N)** dentro del buffer NtQSI | -O(N) → O(log N) en lookups después del capture | **GFR-Ayama-3** |
| **Buffer reuse en steady state** | -allocations completas en hot path | Todas |
| **OpenThread + close en mismo scope** | Evita handle leaks; el cost de open/close ~2 µs juntos < cualquier caching scheme | **GFR-Ayama-2, 4, 5** |
| **`SetThreadIdealProcessor` antes que `SetThreadAffinity` cuando posible** | Soft hint vs hard pin — scheduler retains flexibility, menor riesgo de thread starvation | Ayama-side usage decision |

**Foco real**: integrar enumerate_threads en ProcessMetricsSnapshot
**en vez de** crear otro NtQSI consumer. Eso es ~100 µs ahorrados cada
500ms — escalable.

---

## §1 — GFR-Ayama-2: `gma::hw::set_thread_affinity`

### §1.1 API surface (final)

```cpp
// pillars/topology/include/gamma/topology/HardwareTopology.hpp
namespace gma::hw {
    // Set the CPU affinity mask of an arbitrary thread by TID.
    // Returns the PREVIOUS mask so caller can revert.
    //
    // Windows:  OpenThread(THREAD_SET_INFORMATION |
    //                       THREAD_QUERY_LIMITED_INFORMATION)
    //           + SetThreadAffinityMask + CloseHandle
    // Linux:    sched_setaffinity(tid, ...)  (TID directly, not pthread_t)
    //
    // mask == 0 is rejected (ErrorCode::InvalidArgument) — would suspend the
    // thread permanently, never a legitimate use case.
    //
    // Privilege:
    //   Windows: THREAD_SET_INFORMATION typically granted to admin or the
    //            process that owns the thread. Cross-process admin needed.
    //   Linux:   CAP_SYS_NICE or own-thread.
    [[nodiscard]] std::expected<uint64_t, gma::Error>
    set_thread_affinity(uint32_t tid, uint64_t mask) noexcept;

    // Read current thread affinity without modifying it.
    [[nodiscard]] std::expected<uint64_t, gma::Error>
    get_thread_affinity(uint32_t tid) noexcept;
}
```

**Symmetric con FR-3 (`set_process_affinity`)** — consistent API surface.

### §1.2 Implementation strategy

**Windows**:
```
[[nodiscard]] std::expected<uint64_t, gma::Error>
set_thread_affinity(uint32_t tid, uint64_t mask) noexcept {
    if (mask == 0ull)
        return std::unexpected(gma::Error{gma::ErrorCode::InvalidArgument});

    HANDLE h = OpenThread(
        THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
        FALSE, static_cast<DWORD>(tid));
    if (!h) {
        const DWORD e = GetLastError();
        return std::unexpected(gma::Error{
            e == ERROR_ACCESS_DENIED ? gma::ErrorCode::PermissionDenied
                                     : gma::ErrorCode::InvalidArgument});
    }

    // SetThreadAffinityMask returns the PREVIOUS mask as DWORD_PTR.
    // Returns 0 on failure.
    const DWORD_PTR prev = SetThreadAffinityMask(
        h, static_cast<DWORD_PTR>(mask));
    CloseHandle(h);

    if (prev == 0u)
        return std::unexpected(gma::Error{gma::ErrorCode::IoError});
    return static_cast<uint64_t>(prev);
}
```

**Linux/POSIX**:
```
auto r = sched_setaffinity(tid, sizeof(cpu_set_t), &new_cs);
// Plus reading prev_cs via sched_getaffinity first
```

Same uint64_t ↔ cpu_set_t conversion helper that FR-3 already has — **reuse, don't duplicate**.

### §1.3 Performance characteristics

| Cost | Value |
|---|---|
| Win32 syscall (OpenThread+SetMask+Close) | **~2-3 µs total** |
| C++ wrapper overhead | ~5 ns (single allocation-free function call) |
| Total | **~2-3 µs per call** |

**Frequency of Ayama use**: 2 calls per policy lifecycle (apply + revert).
For B.1 differential pinning: maybe 5-10 threads × 2 = 10-20 calls per
policy. ~50-100 µs of syscall time per policy application. Negligible
vs the 500ms refresh cadence.

### §1.4 No templating (justified)

The function is dominated by the syscall. Templating on the return type
or mask type would add zero performance benefit. The API stays simple.

### §1.5 Test plan

`pillars/topology/tests/thread_affinity_test.cpp`:

1. **Roundtrip on own TID**: get current → set to single-bit mask → verify changed → restore.
2. **Reject mask == 0**: `set_thread_affinity(self_tid, 0)` returns `InvalidArgument`.
3. **Cross-process**: spawn child process via `std::system` background, get its main TID via `enumerate_threads` (once FR-3 ready), pin it, verify, revert.
4. **PermissionDenied**: try to set affinity of a system thread — expect `PermissionDenied`.
5. **Invalid TID**: `set_thread_affinity(0xFFFFFFFE, mask)` → `InvalidArgument` (no such thread).

---

## §2 — GFR-Ayama-3: `gma::proc::enumerate_threads` (THE BIG ONE)

### §2.1 Strategic decision: extend ProcessMetricsSnapshot, not new pillar

**Current state of FR-11 ProcessMetricsSnapshot:**
- Calls `NtQuerySystemInformation(SystemProcessInformation, ...)` once per `capture()`
- Returns ALL processes + ALL their threads in one variable-length buffer
- Existing index: PID-sorted array of byte offsets for O(log N) `find(pid)`

**Insight**: NtQSI already returns thread info as `SYSTEM_THREAD_INFORMATION`
arrays appended to each `SYSTEM_PROCESS_INFORMATION`. **The data is in
the buffer right now and is being ignored.** Adding `extract_threads(pid)`
is just walking deeper into the same buffer.

**Strategy**: Add `extract_threads` method to existing `ProcessMetricsSnapshot`.
NO new pillar. NO new syscall. Just expose the data we already have.

### §2.2 ThreadEntry POD design

```cpp
namespace gma::proc {
    /// Per-thread snapshot data. POD, 48 bytes, alignas(8).
    /// Fits 2 entries per 64-byte cache line.
    struct alignas(8) ThreadEntry {
        uint32_t tid;                  //  4B @0
        uint32_t pid;                  //  4B @4
        uint64_t kernel_time_100ns;    //  8B @8   accumulated kernel CPU
        uint64_t user_time_100ns;      //  8B @16  accumulated user CPU
        uint64_t create_time_100ns;    //  8B @24  thread creation (FILETIME epoch)
        int32_t  priority;             //  4B @32  KPRIORITY (-15..15 typical, 8=normal)
        uint32_t state;                //  4B @36  0=Init 1=Ready 2=Running 3=Standby
                                       //         4=Term 5=Wait 6=Transition 7=Unknown
        uint64_t wait_reason;          //  8B @40  if state==5, why
    };
    static_assert(sizeof(ThreadEntry) == 48u, "ThreadEntry must be 48 bytes");
    static_assert(alignof(ThreadEntry) == 8u, "ThreadEntry must be 8B-aligned");
    static_assert(std::is_trivially_copyable_v<ThreadEntry>);
    static_assert(std::is_standard_layout_v<ThreadEntry>);
}
```

**Layout justification:**
- `tid` first because it's the primary key
- `pid` second for grouping/filtering downstream
- Times grouped together for delta computation
- Priority/state at end for less-frequently-read fields
- 48 bytes = 2 entries per cache line. For typical 50-thread game, 100 threads × 48B = 4.8 KB → fits in L1 entirely.

### §2.3 Extension to ProcessMetricsSnapshot

```cpp
// pillars/process/include/gamma/process/ProcessMetricsSnapshot.hpp
class ProcessMetricsSnapshot {
public:
    // ... existing API (create, capture, find, extract, process_count, data) ...

    /// Bulk extract threads for a given PID. Writes up to `max_count` entries.
    /// Returns count actually written (≤ NumberOfThreads of the process).
    ///
    /// O(log N + threads_for_pid) using the existing PID index. NO syscall
    /// (the data was already captured by capture()).
    ///
    /// Returns 0 if pid not found in last capture.
    [[nodiscard]] uint32_t
    extract_threads(uint32_t pid,
                    ThreadEntry* out,
                    uint32_t max_count) const noexcept;

    /// How many threads the given PID has in the last snapshot.
    /// O(log N). Useful for caller to size buffer.
    [[nodiscard]] uint32_t
    thread_count_for(uint32_t pid) const noexcept;
}
```

### §2.4 Implementation walk-through

`extract_threads(pid, out, max_count)`:

```
1. find(pid) → returns &SYSTEM_PROCESS_INFORMATION for this pid (O(log N))
2. If not found → return 0
3. n_threads = SPI->NumberOfThreads
4. threads_ptr = (SYSTEM_THREAD_INFORMATION*)(SPI + 1)
5. count = min(n_threads, max_count)
6. For i in 0..count:
     STI = threads_ptr[i]
     out[i].tid = (uint32_t)STI->ClientId.UniqueThread
     out[i].pid = pid
     out[i].kernel_time_100ns = STI->KernelTime.QuadPart
     out[i].user_time_100ns   = STI->UserTime.QuadPart
     out[i].create_time_100ns = STI->CreateTime.QuadPart
     out[i].priority          = STI->Priority
     out[i].state             = STI->ThreadState
     out[i].wait_reason       = STI->WaitReason
7. Return count
```

**Cost**: ~50-100 ns per thread + the binary search. For 50 threads:
~5 µs total. **Two orders of magnitude faster than calling NtQSI again.**

### §2.5 Performance characteristics

| Operation | Cost | Notes |
|---|---|---|
| `capture()` | ~80-150 µs | Same as before (NtQSI buffer fill) |
| `find(pid)` | O(log N) ≈ ~50 ns | Binary search in sorted PID index |
| `thread_count_for(pid)` | O(log N) | Same as find + 1 dereference |
| `extract_threads(pid, ...)` | O(log N) + O(threads_for_pid) | ~5 µs for 50 threads |

**Scaling**: 1000-thread system → ~50 µs for one process's threads. Still fast.

### §2.6 Memory implications

NtQSI buffer is already ~256 KB. Adding thread extraction doesn't grow
it — the threads are ALREADY there. The PID index needs no change.

Per-call output: `max_count × 48B`. For 100-thread cap × 48B = 4.8 KB
caller-provided buffer. No heap alloc.

### §2.7 Cross-platform stub

Linux: `/proc/<pid>/task/*` directory walk. Per-thread parsing of
`/proc/<pid>/task/<tid>/stat`. Cost much higher (~1-3 ms per process).

Strategy: implement Linux path but acknowledge in docs that the
performance characteristics differ. Ayama is Windows-first anyway.

### §2.8 Test plan

`pillars/process/tests/enumerate_threads_test.cpp`:

1. **Self enumeration**: `capture()` + `extract_threads(self_pid, ...)`. Verify count > 0, TIDs are non-zero, no duplicates.
2. **Cross-process**: spawn child via `std::system`, `capture()`, `extract_threads(child_pid)`, verify count matches `Process32` reference.
3. **PID not found**: random PID `0xDEADBEEF` → returns 0.
4. **Buffer truncation**: process with 100 threads, max_count=10 → returns exactly 10.
5. **Thread times monotonic**: capture, sleep 100ms, capture, extract twice — kernel_time and user_time should be monotonically increasing.
6. **Thread state validity**: all state values ∈ [0, 7].

---

## §3 — GFR-Ayama-4: `gma::hw::set_thread_ideal_processor`

### §3.1 API surface

```cpp
namespace gma::hw {
    // Set the IDEAL processor for a thread — soft hint, not strict affinity.
    // Scheduler prefers this processor but may move the thread if busy.
    //
    // Use case: combine with set_thread_affinity for "pin to CCD + prefer
    // this specific core within CCD" — gets both isolation and determinism.
    //
    // logical_id == 0xFFFFFFFFu clears the ideal processor (returns to OS default).
    //
    // Returns the previous ideal processor.
    [[nodiscard]] std::expected<uint32_t, gma::Error>
    set_thread_ideal_processor(uint32_t tid, uint32_t logical_id) noexcept;

    [[nodiscard]] std::expected<uint32_t, gma::Error>
    get_thread_ideal_processor(uint32_t tid) noexcept;
}
```

### §3.2 Implementation strategy

**Windows >= 10 / Server 2016**: Use `SetThreadIdealProcessorEx` with
`PROCESSOR_NUMBER` struct (supports >64 logical CPUs via Groups).

```
PROCESSOR_NUMBER proc{};
proc.Group  = static_cast<WORD>(logical_id / 64u);
proc.Number = static_cast<BYTE>(logical_id % 64u);
// SetThreadIdealProcessorEx returns BOOL.
BOOL ok = SetThreadIdealProcessorEx(handle, &proc, &prev_proc);
const uint32_t prev_id =
    static_cast<uint32_t>(prev_proc.Group) * 64u + prev_proc.Number;
```

The non-Ex version (`SetThreadIdealProcessor`) is limited to 64 CPUs and
single group — deprecated since 2008. Use Ex.

### §3.3 Performance

Same ~2-3 µs profile as set_thread_affinity. Same OpenThread/Close
pattern.

### §3.4 Test plan

`pillars/topology/tests/thread_ideal_processor_test.cpp`:

1. Roundtrip on self thread.
2. Verify clearing (passing 0xFFFFFFFF) restores OS default.
3. Reject out-of-range logical_id (≥ topology().logical_core_count()).
4. Cross-process with admin elevation.

---

## §4 — GFR-Ayama-5: `gma::tuning::set_process_working_set`

### §4.1 API surface

```cpp
namespace gma::tuning {
    /// Cross-process variant of set_self_working_set (FR-19).
    /// Sets min/max working set bytes for an arbitrary process.
    ///
    /// Windows: OpenProcess(PROCESS_SET_QUOTA) + SetProcessWorkingSetSizeEx
    /// Linux:   ulimit equivalent (memory rlimit) — limited utility cross-process
    ///
    /// Privilege:
    ///   Windows: PROCESS_SET_QUOTA usually requires SeIncreaseQuotaPrivilege
    ///            for non-owner processes. Admin works.
    [[nodiscard]] std::expected<void, gma::Error>
    set_process_working_set(uint32_t pid,
                            uint64_t min_bytes,
                            uint64_t max_bytes) noexcept;

    [[nodiscard]] std::expected<std::pair<uint64_t, uint64_t>, gma::Error>
    get_process_working_set_limits(uint32_t pid) noexcept;
}
```

### §4.2 Implementation strategy

```
HANDLE h = OpenProcess(PROCESS_SET_QUOTA, FALSE, pid);
if (!h) /* handle error */;

SIZE_T min_sz = static_cast<SIZE_T>(min_bytes);
SIZE_T max_sz = static_cast<SIZE_T>(max_bytes);
BOOL ok = SetProcessWorkingSetSizeEx(h, min_sz, max_sz,
            QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE);
// Soft limits — OS can grow/shrink but tries to respect.
CloseHandle(h);
```

### §4.3 Performance

Same ~2-5 µs profile. Called rarely (once per policy application).

### §4.4 Test plan

`pillars/tuning/tests/working_set_test.cpp` (extend existing):

1. Self process: set min=16MB max=50MB, verify via `GetProcessWorkingSetSizeEx`.
2. Cross-process child: spawn, set limits, verify, cleanup.
3. PermissionDenied for system processes.

---

## §5 — Integration matrix (where each FR lives)

| FR | Pillar | Header | New impl file | Test file | Dep changes |
|---|---|---|---|---|---|
| GFR-Ayama-2 | `topology` | `gamma/topology/HardwareTopology.hpp` (extend FR-3 section) | `pillars/topology/src/HardwareTopology.cpp` (extend) | `pillars/topology/tests/thread_affinity_test.cpp` (NEW) | none |
| GFR-Ayama-3 | `process` | `gamma/process/ProcessMetricsSnapshot.hpp` (extend FR-11) | `pillars/process/src/ProcessMetricsSnapshot.cpp` (extend) | `pillars/process/tests/enumerate_threads_test.cpp` (NEW) | none |
| GFR-Ayama-4 | `topology` | `HardwareTopology.hpp` (same file as FR-3) | `HardwareTopology.cpp` | `thread_ideal_processor_test.cpp` (NEW) | none |
| GFR-Ayama-5 | `tuning` | `gamma/tuning/WorkingSet.hpp` (extend FR-19) | `pillars/tuning/src/WorkingSet.cpp` (extend) | `pillars/tuning/tests/working_set_test.cpp` (extend) | none |
| ImPlot | `ui` | `pillars/ui/CMakeLists.txt` | new dependency only | `pillars/ui/tests/implot_smoke_test.cpp` (NEW) | FetchContent_Declare(implot) |

**No new pillars. No new dependencies (except ImPlot).**

---

## §6 — Cross-cutting: where the strategy DOES pay performance

### §6.1 Shared NtQSI buffer (GFR-Ayama-3)

**This is the single biggest win.**

Without strategy: Ayama would call `NtQSI(SystemProcessInformation, ...)`
twice — once for FR-11 metrics, once for FR-3 threads. Each call: ~100 µs.
At 500ms refresh: ~200 µs per refresh / 0.04% CPU.

With strategy: ONE NtQSI call provides both. Saving: ~100 µs per refresh
or 0.02% sustained CPU. Small absolute but free.

More importantly: keeps the `process` pillar simple — one syscall
consumer, multiple extractors.

### §6.2 Cache-line layout (GFR-Ayama-3)

`ThreadEntry == 48 bytes` was chosen so 2 entries fit per 64-byte cache
line. When Ayama scans thread list to find "main thread" (highest CPU%),
the scan is L1-friendly.

Alternative considered: 64-byte ThreadEntry (one per cache line) with
padding. Rejected because the additional padding doesn't help — we
read SEQUENTIALLY, not random-access, so 2-per-line is cache-optimal
for the access pattern.

### §6.3 PID index reuse (GFR-Ayama-3)

ProcessMetricsSnapshot already has a sorted-by-PID index for O(log N)
`find(pid)`. extract_threads uses the SAME index. No duplicate work.

### §6.4 Win32 handle scoping (FR-2, -4, -5)

All three FRs use the OpenHandle / use / CloseHandle pattern in a
single function scope. No caching, no leaks. Cost: ~1 µs total for
OpenThread+Close. Worth it because:
- Thread lifecycle is unpredictable (threads die)
- Cached handles become stale
- Validation of cached handle costs as much as opening a new one

---

## §7 — Where static polymorphism is **deliberately NOT used**

For the record, I considered and rejected:

1. **Template `set_X_affinity<bool is_thread>`** — would unify thread/
   process variants. Rejected: the two APIs have different Win32 entry
   points (`OpenProcess` vs `OpenThread`) with different access rights;
   the runtime cost is dominated by syscalls; templating would add
   compile-time complexity without ciclo savings.

2. **CRTP for "Pollable<T>" topology accessor** — would unify
   `hw::topology()` cache pattern. Rejected: the cache pattern is
   single-instance, not polymorphic; one function-local static handles
   it perfectly via C++11 thread-safe init.

3. **Concept-constrained `set_thread_property<P>`** — would generalize
   affinity / ideal_processor / priority. Rejected: each Win32 syscall
   is different; the generalization would just be wrappers that defer
   to the specific API anyway.

4. **`constexpr` mask validation** — checking that `mask & ~all_cores`
   is 0 at compile time. Rejected: mask comes from runtime (policy
   decisions), not literals.

---

## §8 — Update plan for GAMMA_INVENTORY.md

After each FR is implemented:

1. Add the new function signature to the relevant pillar's "Public API"
   section.
2. Update the per-pillar status table if the pillar gains a test.
3. Bump the "FR pack" count and add the FR ID to the relevant pillar.

Specifically:

- `topology` pillar gains: `set_thread_affinity`, `get_thread_affinity`,
  `set_thread_ideal_processor`, `get_thread_ideal_processor`. Add test:
  `thread_affinity_test`.
- `process` pillar gains: `ProcessMetricsSnapshot::extract_threads`,
  `ProcessMetricsSnapshot::thread_count_for`, `ThreadEntry` struct. Add
  test: `enumerate_threads_test`.
- `tuning` pillar gains: `set_process_working_set`,
  `get_process_working_set_limits`. Test extended (existing `working_set_test`).
- `ui` pillar gains: ImPlot transitive dependency. Add test:
  `implot_smoke_test`.

---

## §9 — Implementation sequencing

**Recommended order** (lowest risk → highest leverage):

1. **GFR-Ayama-2** `set_thread_affinity` — simplest, symmetric with existing FR-3, low risk, ~1h
2. **GFR-Ayama-4** `set_thread_ideal_processor` — similar pattern, ~1h
3. **GFR-Ayama-3** `extract_threads` — the big one, requires ProcessMetricsSnapshot extension, ~2h
4. **GFR-Ayama-5** `set_process_working_set` — extension of existing FR-19, ~1h
5. **ImPlot integration** — CMake work, ~1.5h

Total: ~6-7 hours of Gamma-side work for the 5 features.

After each FR is implemented + tested + GAMMA_INVENTORY updated, we
can proceed to Ayama-side Phase B work (B.1 uses 2+3+4, B.2 uses 4,
B.3 uses 5).

---

## §10 — Validation checklist before declaring each FR done

Before marking an FR as complete:

- [ ] Public API documented in header with doc comment
- [ ] `noexcept` on the function
- [ ] `[[nodiscard]]` on return value
- [ ] `std::expected<T, gma::Error>` for fallible
- [ ] Cross-platform `#ifdef` paths (Win32 impl + POSIX stub or impl)
- [ ] Test file added under `pillars/<X>/tests/`
- [ ] Test registered in `pillars/<X>/CMakeLists.txt`
- [ ] `ctest` reports the new test PASS
- [ ] `GAMMA_INVENTORY.md` updated with new API + FR-pack ID
- [ ] No new heap allocations in steady state (verified by reading impl)
- [ ] No new system dependencies beyond what the pillar already links
- [ ] If POD struct: `static_assert(sizeof == N)` + trivially copyable

---

*Fin del Implementation Strategy v1.0.*
*Asociado a:* `AYAMA_REFINEMENT_PLAN.md §4`, `GAMMA_INVENTORY.md`,
`AYAMA_MASTER_PLAN.md §0.4`.
