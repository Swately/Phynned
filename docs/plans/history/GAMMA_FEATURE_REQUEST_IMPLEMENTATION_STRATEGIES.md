> **📦 Archived planning document — historical.**
> From Phyriad's pre-rebrand era (the project was codenamed *gamma* / *gma*).
> Published for transparency about how the project was actually built — **not**
> a current specification. Version numbers and performance claims here
> (including any "vs Folly" / "production-ready" framing) reflect the project's
> state at the time of writing and may since have been revised or **retracted**;
> the project has reset to `v0.1.0-experimental`. For current status see the
> repo `README.md` and `docs/planning/`. For benchmark-claim validation status
> see `docs/planning/TEST_INVENTORY.md`.

# Gamma Feature Request — Implementation Strategies
## Planeación exhaustiva para los FRs filed por Ayama v0.1

**Version:** 0.1 — Mayo 2026
**Companion docs:**
- [`GAMMA_FEATURE_REQUESTS.md`](GAMMA_FEATURE_REQUESTS.md) — el catálogo de FRs
- [`AYAMA_IMPLEMENTATION_STRATEGIES.md`](AYAMA_IMPLEMENTATION_STRATEGIES.md) — patrones técnicos compartidos
- [`AYAMA_MASTER_PLAN.md`](AYAMA_MASTER_PLAN.md) — contexto de uso

**Audience:** implementadores Gamma (Sonnet u otros) que recogerán cada FR para integrarlo en los pillares.

**Este documento NO implementa nada.** Su trabajo es:
1. Cerrar todas las decisiones de diseño antes de tocar código.
2. Asegurar que cada API propuesta cumple los estándares Gamma (performance, escalabilidad, comodidad del developer).
3. Trazar las dependencias entre FRs y el orden de adopción.
4. Dejar la verification checklist lista para CI.

---

## Tabla de contenidos

- [§0 — Preámbulo: estándares Gamma no-negociables](#0--preámbulo-estándares-gamma-no-negociables)
- [§1 — FR-1: `gma::hw::topology()` singleton accessor](#1--fr-1-gmahwtopology-singleton-accessor)
- [§2 — FR-2: `HardwareTopology::ccd_count()`](#2--fr-2-hardwaretopologyccd_count)
- [§3 — FR-3: `gma::hw::set_process_affinity()`](#3--fr-3-gmahwset_process_affinity)
- [§4 — FR-4: `gma::proc::enumerate_processes()`](#4--fr-4-gmaprocenumerate_processes)
- [§5 — FR-5: `gma::ipc::ShmRegion<Header, Payload>`](#5--fr-5-gmaipcshmregionheader-payload)
- [§6 — FR-6: `gma::etw::SessionManager`](#6--fr-6-gmaetwsessionmanager)
- [§7 — FR-7: Node hot-restart (future work)](#7--fr-7-node-hot-restart-future-work)
- [§8 — FR-8: Extended `gma::ErrorCode` values ✅](#8--fr-8-extended-gmaerrorcode-values)
- [§9 — FR-9: `gma::hw::set_process_priority()` ✅](#9--fr-9-gmahwset_process_priority)
- [§10 — FR-10: `gma::tuning::check_privilege_level()` ✅](#10--fr-10-gmatuningcheck_privilege_level)
- [§11 — FR-11: `gma::proc::ProcessMetricsSnapshot`](#11--fr-11-gmaprocprocessmetricssnapshot)
- [§12 — FR-12: `gma::ipc::Ring<T>` SPSC ring buffer](#12--fr-12-gmaipcrinct-spsc-ring-buffer)
- [§13 — FR-13: `gma::etw::CtxSwitchSource`](#13--fr-13-gmaetwctxswitchsource)
- [§14 — FR-14: `gma::ui::ForegroundTracker`](#14--fr-14-gmauiforegroundtracker)
- [§15 — FR-15: `gma::config::TomlParser`](#15--fr-15-gmaconfigtomlparser)
- [§16 — FR-16: `gma::sync::WakeEvent`](#16--fr-16-gmasyncwakeevent)
- [§17 — FR-17: `gma::sched::DedicatedThread`](#17--fr-17-gmascheddicatedthread)
- [§18 — FR-18: `gma::proc::CurrentProcess`](#18--fr-18-gmaproccurrentprocess)
- [§19 — FR-19: `gma::tuning::set_self_working_set()`](#19--fr-19-gmatuningset_self_working_set)
- [§20 — Orden de implementación y dependencias](#20--orden-de-implementación-y-dependencias)
- [§21 — Criterios de aceptación (checklist Gamma)](#21--criterios-de-aceptación-checklist-gamma)
- [§22 — Riesgos cross-cutting](#22--riesgos-cross-cutting)
- [§23 — Verification matrix (CI)](#23--verification-matrix-ci)
- [§24 — Open questions a resolver antes de codear](#24--open-questions-a-resolver-antes-de-codear)

---

## §0 — Preámbulo: estándares Gamma no-negociables

Toda implementación de los FRs debe cumplir estos invariantes. Cualquier desviación requiere justificación explícita en el PR description.

### §0.1 — `noexcept` boundary

**Todas** las APIs públicas son `noexcept`. Fallos se reportan vía `std::expected<T, gma::Error>` o un valor sentinel documentado.

```cpp
[[nodiscard]] std::expected<void, gma::Error> foo(...) noexcept;
[[nodiscard]] const HardwareTopology&         bar()    noexcept; // never null
```

Razón: el sistema corre en contexto del agent de Ayama (lo único que se acepta es shutdown ordenado, no `terminate()`).

### §0.2 — Cero allocación dinámica en hot path

Lo que se llama desde el tick loop **NO puede llamar** a `new`, `malloc`, `std::vector::push_back`, `std::string::append`, ni resize de containers. Toda memoria se reserva en `start()` y se libera en `stop()`.

Excepción: APIs explícitamente declaradas "cold path" (load/save de configuración, probe de topología en el inicio).

### §0.3 — POD-friendly por defecto

Tipos que cruzan IPC, SHM o se publican vía `Ring<T>` deben ser:

```cpp
static_assert(std::is_standard_layout_v<T>);
static_assert(std::is_trivially_copyable_v<T>);
static_assert(sizeof(T) % 8u == 0u);          // 8-byte aligned size
static_assert(alignof(T) >= 8u);
```

Strings dentro de PODs son `char[N]` fijos, nunca `std::string`.

### §0.4 — Cache-line awareness

Estructuras hot deben usar `alignas(gma::hal::kDestructivePad)` (típicamente 64B o 128B en x86) cuando hay false sharing potencial entre productor y consumidor:

```cpp
#include <gamma/hal/Cacheline.hpp>
struct alignas(gma::hal::kDestructivePad) ProducerSide { ... };
```

### §0.5 — Singleton-cached probes

Información del sistema que no cambia en runtime (topology, page size, etc.) se computa **una sola vez** mediante `function-local static`:

```cpp
const T& cached() noexcept {
    static const T instance = compute(); // C++11 thread-safe init
    return instance;
}
```

### §0.6 — Pillar topology

Cada FR debe colocarse en el pillar correcto sin introducir dependencias circulares. Orden actual de pillares (leaf → root):

```
hal → schema → transport → topology → scheduler → node → graph
                                                     ↘ runtime → orchestration
                                                       behavior, tuning, daemon
                                                       correlation
render → ui
```

Una nueva API solo puede depender de pillares **anteriores** a ella.

### §0.7 — `§Phase X.Y, gma::pillar` annotation

Todo header nuevo lleva un comment block con su §Phase y pillar, alineado con la convención de los headers existentes (ver `pillars/topology/include/gamma/topology/HardwareTopology.hpp` para referencia).

### §0.8 — ABI stability hint

PODs que cruzan SHM deben llevar un magic value y un version field. Cambios futuros bumpan version. Esto es **obligatorio** para FR-5.

---

## §1 — FR-1: `gma::hw::topology()` singleton accessor

### §1.1 Objetivo

Eliminar la fricción de `std::expected<HardwareTopology, std::string>` para el 99% de consumidores que solo quieren leer la topología. Sustituirla por una referencia constante a un singleton cacheado.

### §1.2 API final

```cpp
// pillars/topology/include/gamma/topology/HardwareTopology.hpp
namespace gma::hw {

/// Returns the cached process-singleton HardwareTopology.
///
/// On first call: invokes HardwareTopology::probe() and caches the result.
/// On probe failure: returns a sentinel empty topology (cores.empty() == true).
/// Subsequent calls: O(1) — single atomic load + pointer return.
///
/// Thread-safe (C++11 function-local static guarantee).
[[nodiscard]] const HardwareTopology& topology() noexcept;

/// Returns the error message from the most recent probe attempt, or empty
/// string if the probe succeeded. Empty string is the success indicator.
///
/// Useful for diagnostics when topology().cores is unexpectedly empty.
[[nodiscard]] std::string_view last_probe_error() noexcept;

} // namespace gma::hw
```

### §1.3 Pillar placement

`pillars/topology` — extensión natural del módulo que ya hostea `HardwareTopology::probe()`.

### §1.4 Strategy

Implementación en 30 líneas de `HardwareTopology.cpp`:

```cpp
namespace {
struct TopologyCache {
    HardwareTopology topo;
    std::string      err;
};

const TopologyCache& cache() noexcept {
    static const TopologyCache c = []() noexcept {
        TopologyCache tmp;
        auto r = HardwareTopology::probe();
        if (r) tmp.topo = std::move(*r);
        else   tmp.err  = std::move(r.error());
        return tmp;
    }();
    return c;
}
} // anonymous

namespace gma::hw {
const HardwareTopology& topology() noexcept   { return cache().topo; }
std::string_view        last_probe_error() noexcept { return cache().err; }
}
```

### §1.5 Performance profile

| Métrica | Valor |
|---|---|
| First call | ~5-20 ms (probe cost — CPUID, registry reads, /proc parsing) |
| Subsequent calls | ~2 ns (atomic load + return) |
| Heap allocation | Solo durante primera probe; ninguna después |
| Branch prediction | Single branch on first call only |

### §1.6 Threading

- `function-local static` provee inicialización segura en hilos múltiples (C++11).
- Después de la primera llamada, todos los accesos son lock-free read-only.
- La `HardwareTopology` retornada es **inmutable** después de probe.

### §1.7 Sentinel empty topology

Si `probe()` falla, `cache().topo` queda en su estado default-constructed:
- `cores.empty()` → true
- `physical_core_count() == 0`
- `logical_core_count() == 0`
- Todos los `hw::*_cores()` retornan vectores vacíos

Consumidores deben **siempre** validar `!topo.cores.empty()` antes de usar la información. Esto es lo mismo que ya hacen al chequear `r.has_value()`, solo que en un sitio más natural.

### §1.8 Tests requeridos

1. `topology()` segunda llamada retorna referencia idéntica (`==&` pointer comparison).
2. `topology()` después de probe exitoso → `cores.size() == hw::enumerate_cores().size()`.
3. `last_probe_error()` empty cuando probe succeed.
4. `topology()` thread-safe — concurrent llamadas desde 8 threads no causan UB (TSan clean).
5. `topology()` no allocates después de la primera llamada (medido vía custom allocator counter).

### §1.9 Migración Ayama

En `AgentRuntime::start()`:

```cpp
// ANTES (con FR pendiente):
const auto topo = gma::hw::probe_topology();   // <- bug latente
impl_->policy_engine.register_default_rules(topo);

// DESPUÉS (post-FR):
const gma::HardwareTopology& topo = gma::hw::topology();
if (topo.cores.empty()) {
    std::fprintf(stderr, "[Ayama] Topology probe failed: %.*s\n",
        static_cast<int>(gma::hw::last_probe_error().size()),
        gma::hw::last_probe_error().data());
    return std::unexpected(gma::Error{gma::ErrorCode::SystemError});
}
impl_->policy_engine.register_default_rules(topo);
```

### §1.10 Estimación

- **Tamaño:** S (≤80 LOC + 5 tests)
- **Dependencias:** ninguna (modifica solo `topology` pillar)
- **Riesgo:** muy bajo (additive, no rompe APIs existentes)

---

## §2 — FR-2: `HardwareTopology::ccd_count()`

### §2.1 Objetivo

Métrica trivial pero recomputada en al menos 3 sitios distintos en Ayama (`AutoPolicySelector::classify`, `AutoPolicySelector::build_masks`, `PolicyEngine::register_default_rules`). Cachear una vez en `probe()`.

### §2.2 API final

```cpp
// pillars/topology/include/gamma/topology/HardwareTopology.hpp
struct HardwareTopology {
    // ... campos existentes ...

    /// Number of distinct CCD/CCX IDs in `cores`.
    /// Returns 0 if cores is empty. Returns 1 for single-CCD CPUs.
    /// Computed once during probe() and cached.
    [[nodiscard]] uint32_t ccd_count() const noexcept { return ccd_count_; }

private:
    uint32_t ccd_count_{0u};   // populated by probe()
    // ...
};
```

### §2.3 Strategy

En `HardwareTopology::probe()`, después de poblar `cores`:

```cpp
uint32_t max_ccd = 0u;
bool has_any = false;
for (const auto& c : topo.cores) {
    if (c.ccd_id > max_ccd) max_ccd = c.ccd_id;
    has_any = true;
}
topo.ccd_count_ = has_any ? (max_ccd + 1u) : 0u;
```

### §2.4 Memory cost

+4 bytes por `HardwareTopology`. Insignificante (la struct ya pesa varios KB por los `std::vector`).

### §2.5 Performance

- O(N) durante probe (una vez en lifetime del proceso).
- O(1) en cada `ccd_count()` (return de campo cacheado).

### §2.6 Tests requeridos

1. Single-CCD CPU (5600X, 7600X): `ccd_count() == 1`.
2. Dual-CCD CPU (5950X, 7950X): `ccd_count() == 2`.
3. Empty topology (sentinel): `ccd_count() == 0`.
4. Compile-time check: `noexcept(topo.ccd_count())`.

### §2.7 Migración Ayama

Reemplazar las 3+ ocurrencias del patrón manual:

```cpp
// ANTES:
uint32_t max_ccd = 0u;
for (const auto& c : topo.cores)
    if (c.ccd_id > max_ccd) max_ccd = c.ccd_id;
const uint32_t n_ccd = topo.cores.empty() ? 0u : max_ccd + 1u;

// DESPUÉS:
const uint32_t n_ccd = topo.ccd_count();
```

Sitios a actualizar:
- `ayama/policy/include/ayama/policy/AutoPolicySelector.hpp::classify()`
- `ayama/policy/include/ayama/policy/AutoPolicySelector.hpp::build_masks()`
- `ayama/policy/src/PolicyEngine.cpp::register_default_rules()`

### §2.8 Estimación

- **Tamaño:** XS (≤20 LOC + 4 tests + 3 migrations)
- **Dependencias:** ninguna
- **Riesgo:** nulo (additive)

---

## §3 — FR-3: `gma::hw::set_process_affinity()`

### §3.1 Objetivo

Centralizar la afinidad a nivel de proceso (PID externo, no hilo propio) en `hw::`. Ayama actualmente implementa esto en `ActionExecutor`; cualquier otra herramienta Gamma que quiera tocar afinidad terminará duplicando código.

### §3.2 API final

```cpp
// pillars/topology/include/gamma/topology/HardwareTopology.hpp
namespace gma::hw {

/// Set the CPU affinity mask of an arbitrary process.
///
/// `pid`  — target process ID (must be running and accessible).
/// `mask` — bitmask of allowed logical cores. 0 = no cores (invalid).
///
/// Windows: requires PROCESS_SET_INFORMATION. Caller usually needs admin
/// elevation to affect processes from other users.
/// Linux: requires CAP_SYS_NICE (or root) to affect processes owned by others.
///
/// Returns previous mask on success for trivial revert; gma::Error on failure.
[[nodiscard]] std::expected<uint64_t, gma::Error>
set_process_affinity(uint32_t pid, uint64_t mask) noexcept;

/// Read the current affinity mask of `pid`. Useful for capturing previous
/// state before applying a change.
[[nodiscard]] std::expected<uint64_t, gma::Error>
get_process_affinity(uint32_t pid) noexcept;

} // namespace gma::hw
```

### §3.3 Pillar placement

`pillars/topology` — junto a `pin_current_thread()` que ya existe ahí.

### §3.4 Strategy: Windows

```cpp
std::expected<uint64_t, gma::Error>
set_process_affinity(uint32_t pid, uint64_t mask) noexcept
{
    if (mask == 0ull)
        return std::unexpected(gma::Error{gma::ErrorCode::InvalidArgument,
            "affinity mask must be non-zero"});

    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, pid);
    if (!h) {
        const DWORD err = GetLastError();
        return std::unexpected(gma::Error{
            err == ERROR_ACCESS_DENIED ? gma::ErrorCode::PermissionDenied
                                       : gma::ErrorCode::SystemError,
            "OpenProcess failed"});
    }

    // Capture previous mask BEFORE changing.
    DWORD_PTR prev_proc_mask = 0;
    DWORD_PTR system_mask    = 0;
    if (!GetProcessAffinityMask(h, &prev_proc_mask, &system_mask)) {
        CloseHandle(h);
        return std::unexpected(gma::Error{gma::ErrorCode::SystemError,
            "GetProcessAffinityMask failed"});
    }

    if (!SetProcessAffinityMask(h, static_cast<DWORD_PTR>(mask))) {
        CloseHandle(h);
        return std::unexpected(gma::Error{gma::ErrorCode::SystemError,
            "SetProcessAffinityMask failed"});
    }

    CloseHandle(h);
    return static_cast<uint64_t>(prev_proc_mask);
}
```

### §3.5 Strategy: Linux

```cpp
std::expected<uint64_t, gma::Error>
set_process_affinity(uint32_t pid, uint64_t mask) noexcept
{
    cpu_set_t prev_set;
    CPU_ZERO(&prev_set);
    if (sched_getaffinity(static_cast<pid_t>(pid), sizeof(prev_set), &prev_set) != 0) {
        return std::unexpected(gma::Error{
            errno == EPERM ? gma::ErrorCode::PermissionDenied
                           : gma::ErrorCode::SystemError,
            "sched_getaffinity failed"});
    }

    cpu_set_t new_set;
    CPU_ZERO(&new_set);
    for (uint32_t i = 0u; i < 64u; ++i) {
        if (mask & (1ull << i)) CPU_SET(i, &new_set);
    }

    if (sched_setaffinity(static_cast<pid_t>(pid), sizeof(new_set), &new_set) != 0) {
        return std::unexpected(gma::Error{
            errno == EPERM ? gma::ErrorCode::PermissionDenied
                           : gma::ErrorCode::SystemError,
            "sched_setaffinity failed"});
    }

    // Convert prev_set back to uint64_t.
    uint64_t prev_mask = 0ull;
    for (uint32_t i = 0u; i < 64u; ++i) {
        if (CPU_ISSET(i, &prev_set)) prev_mask |= (1ull << i);
    }
    return prev_mask;
}
```

### §3.6 Error code mapping

| OS error | `gma::ErrorCode` |
|---|---|
| ERROR_ACCESS_DENIED / EPERM | `PermissionDenied` |
| ERROR_INVALID_PARAMETER / EINVAL | `InvalidArgument` |
| ERROR_INVALID_HANDLE / ESRCH | `NotFound` |
| Otros | `SystemError` |

### §3.7 Performance

- ~1-5 µs por llamada (1-2 syscalls).
- **No es hot path**: Ayama llama esto solo cuando un policy decide aplicar/revertir affinity. Frecuencia esperada: <1 vez/segundo en estado estable.

### §3.8 64-vs-more-cores

`uint64_t` cubre hasta 64 cores lógicos. Para sistemas con >64 cores (Threadripper Pro 7995WX = 192 logical), considerar futuro `set_process_affinity_groups()` con `GROUP_AFFINITY` en Windows. **Out of scope para v1**: ningún CPU consumer/prosumer al que Ayama apunta tiene >64 logical cores.

### §3.9 Tests requeridos

1. Aplicar affinity al propio proceso de test → leer con `get_process_affinity()` → debe coincidir.
2. Aplicar mask=0 → retornar `InvalidArgument`.
3. PID inválido (e.g. 999999999) → retornar `NotFound`.
4. PID de otro usuario sin elevación → `PermissionDenied` (skip en CI sin sudo).
5. Roundtrip: capturar prev, aplicar new, restaurar prev, leer → debe coincidir.

### §3.10 Migración Ayama

`ayama/action/src/ActionExecutor.cpp` actualmente tiene ~40 LOC platform-specific para esto. Reemplazar por:

```cpp
auto prev = gma::hw::set_process_affinity(d.target_pid, d.core_mask);
if (!prev) {
    log_error(prev.error());
    return std::unexpected(prev.error());
}
entry.prev_affinity_mask = *prev;   // captured atomically with set
```

Beneficio adicional: `GetProcessAffinityMask + SetProcessAffinityMask` ahora atómico al consumidor (la implementación garantiza orden correcto).

### §3.11 Estimación

- **Tamaño:** M (~150 LOC en `HardwareTopology.cpp` con plataforma split + 5 tests)
- **Dependencias:** ninguna nueva (ya está `<windows.h>` / `<sched.h>` en topology.cpp)
- **Riesgo:** bajo. El syscall ya es bien conocido; el envoltorio es trivial.

---

## §4 — FR-4: `gma::proc::enumerate_processes()`

### §4.1 Objetivo

Enumeración de procesos cross-platform sin allocación dinámica. Ayama lo necesita cada tick (cuando hay targets activos: 100 ms). Otras herramientas Gamma (futuro process-aware scheduler) lo necesitarán también.

### §4.2 Decisión de API: caller-allocated buffer

Para evitar heap allocation en hot path, la API **no retorna `std::vector`**. El caller provee un buffer pre-allocado:

```cpp
// pillars/process/include/gamma/process/ProcessEnumerator.hpp
namespace gma::proc {

/// Lightweight process descriptor — POD, 80 bytes (cache-friendly).
struct alignas(8) ProcessEntry {
    uint32_t pid;            //  4B
    uint32_t parent_pid;     //  4B
    uint64_t start_time;     //  8B  — TSC equivalent (FILETIME on Win, jiffies on Linux)
    char     name[64];       // 64B  — null-terminated exe short name (ASCII)
};
static_assert(sizeof(ProcessEntry)  == 80u);
static_assert(alignof(ProcessEntry) == 8u);
static_assert(std::is_trivially_copyable_v<ProcessEntry>);

inline constexpr uint32_t kMaxProcesses = 1024u;  // soft upper bound

/// Fill `out[0..max_count)` with currently running processes.
/// Returns the count written (≤ max_count). On error: returns 0 and sets
/// last error retrievable via last_enumerate_error().
///
/// Performance: ~1 ms for 200 procs on Win10, ~300 µs for 200 procs on Linux.
/// No heap allocation. Single syscall (Windows EnumProcesses) or
/// readdir loop on /proc (Linux).
[[nodiscard]] uint32_t enumerate_processes(
    ProcessEntry* out, uint32_t max_count) noexcept;

/// Diagnostic accessor. Returns empty string_view if the last call succeeded.
[[nodiscard]] std::string_view last_enumerate_error() noexcept;

} // namespace gma::proc
```

### §4.3 Pillar placement

**Decisión:** Nuevo pillar `pillars/process/` o módulo dentro de `pillars/behavior`.

Recomendación: **nuevo pillar `pillars/process`** porque:
1. Aísla la dependencia con `psapi.lib` (Windows) y `/proc` (Linux).
2. Permite a Ayama y otros consumers depender de `gamma_process` sin arrastrar todo `gamma_behavior`.
3. Mantiene la separación: behavior = comportamiento del runtime; process = información del sistema externa.

Dependencias del pillar: `gamma_hal` (timestamps), `gamma_schema` (Error).

### §4.4 Strategy: Windows

```cpp
uint32_t enumerate_processes(ProcessEntry* out, uint32_t max_count) noexcept
{
    if (!out || max_count == 0u) return 0u;

    // EnumProcesses needs a pre-allocated buffer of DWORD PIDs.
    // We use a thread-local fixed buffer to avoid heap allocation.
    thread_local DWORD pids[kMaxProcesses];
    DWORD bytes_returned = 0;

    if (!EnumProcesses(pids, sizeof(pids), &bytes_returned)) {
        set_last_error("EnumProcesses failed");
        return 0u;
    }

    const uint32_t n_pids = std::min<uint32_t>(
        bytes_returned / sizeof(DWORD), max_count);

    uint32_t out_count = 0u;
    for (uint32_t i = 0u; i < n_pids && out_count < max_count; ++i) {
        const DWORD pid = pids[i];
        if (pid == 0u) continue;  // System Idle

        // Open process with minimum rights: QUERY_LIMITED_INFORMATION
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, pid);
        if (!h) continue;  // protected process — skip silently

        ProcessEntry& e = out[out_count];
        e.pid        = pid;
        e.parent_pid = 0u;   // expensive on Windows — leave 0; use NtQuery if needed
        e.start_time = 0u;   // populate via GetProcessTimes if needed
        e.name[0]    = '\0';

        // QueryFullProcessImageNameA → wide path → basename
        char path[MAX_PATH];
        DWORD path_len = MAX_PATH;
        if (QueryFullProcessImageNameA(h, 0, path, &path_len)) {
            // Extract basename
            const char* base = path;
            for (DWORD i = 0; i < path_len; ++i)
                if (path[i] == '\\' || path[i] == '/') base = path + i + 1;
            std::strncpy(e.name, base, sizeof(e.name) - 1u);
            e.name[sizeof(e.name) - 1u] = '\0';
        }

        // Optional: GetProcessTimes for start_time
        FILETIME ft_create, ft_exit, ft_kernel, ft_user;
        if (GetProcessTimes(h, &ft_create, &ft_exit, &ft_kernel, &ft_user)) {
            e.start_time =
                (static_cast<uint64_t>(ft_create.dwHighDateTime) << 32) |
                ft_create.dwLowDateTime;
        }

        CloseHandle(h);
        ++out_count;
    }
    return out_count;
}
```

**Optimización avanzada (futuro):** usar `NtQuerySystemInformation(SystemProcessInformation, ...)` que retorna toda la info (PID, PPID, name, start time) en una sola syscall sin necesidad de `OpenProcess` por PID. Esto bajaría el costo de ~1 ms a ~100 µs.

### §4.5 Strategy: Linux

```cpp
uint32_t enumerate_processes(ProcessEntry* out, uint32_t max_count) noexcept
{
    DIR* d = opendir("/proc");
    if (!d) { set_last_error("opendir(/proc) failed"); return 0u; }

    uint32_t out_count = 0u;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr && out_count < max_count) {
        // Skip non-numeric entries
        char* endp = nullptr;
        const unsigned long pid_ul = std::strtoul(entry->d_name, &endp, 10);
        if (*endp != '\0') continue;

        ProcessEntry& e = out[out_count];
        e.pid        = static_cast<uint32_t>(pid_ul);
        e.parent_pid = 0u;
        e.start_time = 0u;
        e.name[0]    = '\0';

        // Read /proc/<pid>/comm for short name
        char comm_path[64];
        std::snprintf(comm_path, sizeof(comm_path),
                      "/proc/%s/comm", entry->d_name);
        int fd = ::open(comm_path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = ::read(fd, e.name, sizeof(e.name) - 1u);
            ::close(fd);
            if (n > 0) {
                // /proc/<pid>/comm has trailing newline
                if (e.name[n - 1] == '\n') --n;
                e.name[n] = '\0';
            }
        }

        // Optional: parse /proc/<pid>/stat for parent_pid and start_time
        // (more expensive — skip in first iteration)

        ++out_count;
    }
    closedir(d);
    return out_count;
}
```

### §4.6 Performance profile

| Caso | Latencia (50 procs) | Latencia (300 procs) |
|---|---|---|
| Win10 `EnumProcesses + OpenProcess` por PID | 800 µs | 2.5 ms |
| Win10 `NtQuerySystemInformation` (futuro) | 100 µs | 300 µs |
| Linux `readdir(/proc)` + `comm` only | 200 µs | 700 µs |
| Linux `readdir + comm + stat` | 600 µs | 2 ms |

Ayama llama esto cada 100 ms (Active state). 2.5 ms / 100 ms = 2.5% CPU peak — dentro del budget anti-parasitario.

### §4.7 Diferencia con `ProcessInfoProvider` mencionado en FR-4

El FR original sugirió retornar `std::vector<ProcessEntry>`. Pero el patrón Gamma de zero-alloc fuerza la API de buffer pre-allocado. El nombre del subnamespace `proc::` es preferible a `ProcessInfoProvider` (clase) porque:

1. Es solo funciones libres.
2. Sin estado mutable global → trivialmente thread-safe.
3. Compatible con el patrón `hw::` existente.

### §4.8 Tests requeridos

1. Auto-test: `enumerate_processes()` debe incluir `getpid()` propio.
2. Buffer pequeño: pasar `max_count=5` debe truncar correctamente, sin overflow.
3. Buffer cero: `max_count=0` retorna 0.
4. Auto-name: el `name` del PID propio debe terminar en "_test" o similar.
5. No heap: medir con counter custom que la función no allocates.

### §4.9 Migración Ayama

`ayama/observer/src/ProcessObserver.cpp` actualmente tiene ~60 LOC platform-specific. Reemplazar por:

```cpp
void ProcessObserver::refresh() noexcept {
    static thread_local gma::proc::ProcessEntry buf[gma::proc::kMaxProcesses];
    const uint32_t n = gma::proc::enumerate_processes(buf, gma::proc::kMaxProcesses);
    // Filter into kMaxTargets, classify, etc.
}
```

### §4.10 Estimación

- **Tamaño:** L (~250 LOC + nuevo CMakeLists para pillar + 5 tests + Linux/Win split)
- **Dependencias:** `gamma_hal`, `gamma_schema`
- **Riesgo:** medio. Windows path es bien probado; Linux `/proc` parsing tiene casos edge (procesos efímeros entre readdir y open). Manejar `ENOENT` como skip silencioso.

---

## §5 — FR-5: `gma::ipc::ShmRegion<Header, Payload>`

### §5.1 Objetivo

Plantilla type-safe para compartir memoria entre procesos con seqlock incluido. Reemplaza ~300 LOC de `AyamaAgentPublisher.cpp` + ~150 LOC de `AyamaClient.cpp` por dos especializaciones.

### §5.2 API final

```cpp
// pillars/ipc/include/gamma/ipc/ShmRegion.hpp
namespace gma::ipc {

/// Required header contract — Header must include these fields in this order:
///   uint32_t magic;
///   uint32_t version;
///   std::atomic<uint32_t> seq;
///   std::atomic<uint32_t> agent_pid;
/// Compile-time enforced via the ShmHeaderConcept concept.
template <typename T>
concept ShmHeaderConcept = requires(T h) {
    { h.magic     } -> std::convertible_to<uint32_t>;
    { h.version   } -> std::convertible_to<uint32_t>;
    { h.seq       } -> std::convertible_to<std::atomic<uint32_t>&>;
    { h.agent_pid } -> std::convertible_to<std::atomic<uint32_t>&>;
};

/// Typed shared-memory region with seqlock-based read/write coordination.
///
/// Layout: [Header][Payload] back-to-back in a single mapping.
///
/// Producer:
///   auto r = ShmRegion<H,P>::create("Local\\MyApp.v1", /*magic=*/0xAB12);
///   r->header().seq.fetch_add(1, ...);   // odd = writing
///   r->payload() = new_data;
///   r->header().seq.fetch_add(1, ...);   // even = consistent
///
/// Consumer:
///   auto r = ShmRegion<H,P>::open("Local\\MyApp.v1", /*magic=*/0xAB12);
///   // Use the helper for consistent reads:
///   P snapshot;
///   if (r->try_read_consistent(&snapshot)) { ... }
template <ShmHeaderConcept Header, typename Payload>
class ShmRegion {
public:
    static_assert(std::is_standard_layout_v<Header>);
    static_assert(std::is_standard_layout_v<Payload>);
    static_assert(std::is_trivially_copyable_v<Payload>);
    static_assert(alignof(Header)  >= 8u);
    static_assert(alignof(Payload) >= 8u);

    /// Create or truncate as producer. Owner — calls shm_unlink on close (POSIX).
    [[nodiscard]] static std::expected<ShmRegion, gma::Error>
    create(const char* name, uint32_t magic, uint32_t version = 1u) noexcept;

    /// Open as consumer. Read-only mapping. Non-owner.
    [[nodiscard]] static std::expected<ShmRegion, gma::Error>
    open(const char* name, uint32_t magic) noexcept;

    ShmRegion(ShmRegion&&) noexcept;
    ShmRegion& operator=(ShmRegion&&) noexcept;
    ~ShmRegion() noexcept { close(); }

    ShmRegion(const ShmRegion&)            = delete;
    ShmRegion& operator=(const ShmRegion&) = delete;

    void close() noexcept;
    [[nodiscard]] bool is_open()  const noexcept { return ptr_ != nullptr; }
    [[nodiscard]] bool is_owner() const noexcept { return is_owner_; }

    // ── Producer-side: direct access (mutable Header + Payload) ───────────────
    [[nodiscard]] Header&  header()  noexcept;
    [[nodiscard]] Payload& payload() noexcept;

    /// RAII guard that bumps seq odd→even around a critical section.
    /// Usage:
    ///   { auto g = region->begin_write(); region->payload() = new_data; }
    [[nodiscard]] auto begin_write() noexcept;

    // ── Consumer-side: const accessors + seqlock read helper ──────────────────
    [[nodiscard]] const Header&  header()  const noexcept;
    [[nodiscard]] const Payload& payload() const noexcept;

    /// Try to read a consistent snapshot via seqlock retry (max 3 attempts).
    /// Returns false if 3 consecutive reads were torn (use last good snapshot).
    [[nodiscard]] bool try_read_consistent(Payload* out) const noexcept;

private:
    void*    ptr_      {nullptr};
    size_t   size_     {0u};
    bool     is_owner_ {false};
    // platform handles + name buffer
#ifdef _WIN32
    HANDLE   mapping_  {nullptr};
#else
    int      fd_       {-1};
    char     name_buf_[64]{};
#endif
};

} // namespace gma::ipc
```

### §5.3 Pillar placement

**Nuevo pillar:** `pillars/ipc`. Justificación:

- IPC no encaja en `transport` (que es in-process Ring) ni en `process` (que es read-only enumeration).
- Pillar `ipc` puede crecer en el futuro con `NamedPipe`, `UnixSocket`, etc.
- Dependencias: `gamma_hal`, `gamma_schema`. Sin dependencia de `topology` ni `behavior`.

### §5.4 Strategy: Windows

```cpp
template <ShmHeaderConcept H, typename P>
std::expected<ShmRegion<H,P>, gma::Error>
ShmRegion<H,P>::create(const char* name, uint32_t magic, uint32_t version) noexcept
{
    constexpr size_t kSize = sizeof(H) + sizeof(P);

    // Convert to wide string (file mapping names are wide on Win).
    wchar_t wname[256];
    if (MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 256) == 0) {
        return std::unexpected(gma::Error{gma::ErrorCode::InvalidArgument});
    }

    HANDLE map = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, static_cast<DWORD>(kSize), wname);
    if (!map) return std::unexpected(gma::Error{gma::ErrorCode::SystemError});

    void* p = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, kSize);
    if (!p) { CloseHandle(map);
              return std::unexpected(gma::Error{gma::ErrorCode::SystemError}); }

    // Zero-init the header+payload (memset is fine for trivial types).
    std::memset(p, 0, kSize);

    H* hdr = static_cast<H*>(p);
    hdr->version   = version;
    hdr->agent_pid.store(static_cast<uint32_t>(GetCurrentProcessId()),
                         std::memory_order_release);
    // Write magic LAST so consumers gate on it.
    std::atomic_thread_fence(std::memory_order_release);
    hdr->magic = magic;

    ShmRegion r;
    r.ptr_      = p;
    r.size_     = kSize;
    r.is_owner_ = true;
    r.mapping_  = map;
    return r;
}
```

### §5.5 Strategy: POSIX

```cpp
template <ShmHeaderConcept H, typename P>
std::expected<ShmRegion<H,P>, gma::Error>
ShmRegion<H,P>::create(const char* name, uint32_t magic, uint32_t version) noexcept
{
    constexpr size_t kSize = sizeof(H) + sizeof(P);

    // POSIX shm names must start with '/' and contain no other '/'.
    char canonical[64];
    if (name[0] == '/') std::strncpy(canonical, name, sizeof(canonical) - 1u);
    else std::snprintf(canonical, sizeof(canonical), "/%s", name);

    int fd = shm_open(canonical, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) return std::unexpected(gma::Error{gma::ErrorCode::SystemError});
    if (ftruncate(fd, static_cast<off_t>(kSize)) != 0) {
        ::close(fd); shm_unlink(canonical);
        return std::unexpected(gma::Error{gma::ErrorCode::SystemError});
    }
    void* p = mmap(nullptr, kSize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        ::close(fd); shm_unlink(canonical);
        return std::unexpected(gma::Error{gma::ErrorCode::SystemError});
    }

    std::memset(p, 0, kSize);
    H* hdr = static_cast<H*>(p);
    hdr->version   = version;
    hdr->agent_pid.store(static_cast<uint32_t>(getpid()),
                         std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    hdr->magic = magic;

    ShmRegion r;
    r.ptr_      = p;
    r.size_     = kSize;
    r.is_owner_ = true;
    r.fd_       = fd;
    std::strncpy(r.name_buf_, canonical, sizeof(r.name_buf_) - 1u);
    return r;
}
```

### §5.6 Seqlock helpers

```cpp
template <ShmHeaderConcept H, typename P>
auto ShmRegion<H,P>::begin_write() noexcept {
    struct WriteGuard {
        H* hdr;
        WriteGuard(H* h) noexcept : hdr(h) {
            hdr->seq.fetch_add(1, std::memory_order_acq_rel);  // odd
        }
        ~WriteGuard() noexcept {
            hdr->seq.fetch_add(1, std::memory_order_acq_rel);  // even
        }
        WriteGuard(WriteGuard&&) = delete;
    };
    return WriteGuard{&header()};
}

template <ShmHeaderConcept H, typename P>
bool ShmRegion<H,P>::try_read_consistent(P* out) const noexcept {
    const H& hdr = header();
    const P& src = payload();
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t s0 = hdr.seq.load(std::memory_order_acquire);
        if (s0 & 1u) continue;  // writer in progress
        std::memcpy(out, &src, sizeof(P));
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s1 = hdr.seq.load(std::memory_order_acquire);
        if (s0 == s1) return true;
    }
    return false;
}
```

### §5.7 Performance

- `create()`: ~50-200 µs (CreateFileMappingW/shm_open + mmap)
- `header()`/`payload()`: zero cost (inlined pointer arithmetic)
- `begin_write()`: 2 atomic fetch_add (no contention con single-writer)
- `try_read_consistent()`: 2 atomic loads + memcpy (~size/16 ns on x86)

### §5.8 ABI compatibility con magic + version

`open()` valida:

```cpp
if (hdr->magic != expected_magic) {
    return std::unexpected(gma::Error{gma::ErrorCode::InvalidFormat,
        "magic mismatch"});
}
if (hdr->version != expected_version) {
    return std::unexpected(gma::Error{gma::ErrorCode::VersionMismatch,
        "version mismatch"});
}
```

Esto da future-proofing: cuando Ayama 0.2 bumpa version, los clientes 0.1 fallan al `open()` con un error claro en lugar de leer datos corruptos.

### §5.9 Tests requeridos

1. POD layout: `static_assert` que Header concept se cumple para AyamaStateHeader.
2. Create + open roundtrip in-process (fork test).
3. Seqlock torn-read detection: forzar un write durante read.
4. Magic mismatch: open con magic equivocado → `InvalidFormat`.
5. Version mismatch: open con version diferente → `VersionMismatch`.
6. Owner shm_unlink: crear en proceso, terminar, verificar shm gone (POSIX).
7. Concurrent reads: 8 threads consumers + 1 producer → ningún torn read sin detect.

### §5.10 Migración Ayama

`ayama/ipc/include/ayama/ipc/AyamaAgentPublisher.hpp`:

```cpp
// ANTES (~300 LOC):
class AyamaAgentPublisher {
    HANDLE map_handle_;
    void*  ptr_;
    // ... raw mmap/MapViewOfFile ...
};

// DESPUÉS (~50 LOC):
class AyamaAgentPublisher {
    gma::ipc::ShmRegion<AyamaShmHeader, AyamaPayload> region_;
public:
    bool open(const char* name, uint32_t pid) noexcept {
        auto r = gma::ipc::ShmRegion<...>::create(name, kAyamaMagic);
        if (!r) return false;
        region_ = std::move(*r);
        region_.header().agent_pid.store(pid, std::memory_order_release);
        return true;
    }
    void publish(...) noexcept {
        auto guard = region_.begin_write();
        // Mutate region_.payload() fields
    }
};
```

### §5.11 Estimación

- **Tamaño:** L (~500 LOC con templates + plataforma + 7 tests)
- **Dependencias:** `gamma_hal` (Cacheline, MemoryOrder), `gamma_schema` (Error)
- **Riesgo:** medio-alto. Template + concept + cross-platform = surface grande. Mitigación: copiar el patrón existente de `AyamaAgentPublisher.cpp` que ya funciona y ya pasó stress test en producción.

---

## §6 — FR-6: `gma::etw::SessionManager`

### §6.1 Objetivo

Abstraer la creación/teardown de sesiones ETW Windows. Ayama actualmente lo tiene embebido en `MetricsCollector` con manejo manual de `StartTrace`, `EnableTraceEx2`, `OpenTrace`, `ProcessTrace`, en otros 200+ LOC mezclados con la lógica de sampling.

### §6.2 API final

```cpp
// pillars/etw/include/gamma/etw/SessionManager.hpp
// Windows-only pillar. On non-Windows: header defines no-op stubs.
namespace gma::etw {

#ifdef _WIN32

/// Single ETW provider to enable on a session.
struct ProviderSpec {
    GUID      guid;          // Provider GUID (e.g. kKernelProcessProvider)
    UCHAR     level;         // TRACE_LEVEL_INFORMATION etc.
    ULONGLONG match_any;     // Keyword mask — events with ANY of these keywords
    ULONGLONG match_all;     // Additional ALL-keyword filter (usually 0)
};

/// Event consumption callback. Called from the trace consumer thread.
/// MUST be lightweight: copy data and return. Heavy work goes in app code.
using EventCallback = void (*)(const EVENT_RECORD& rec, void* user_ctx) noexcept;

/// One ETW session managing multiple providers + a consumer thread.
class SessionManager {
public:
    SessionManager() noexcept = default;
    ~SessionManager() noexcept { stop(); }

    SessionManager(const SessionManager&)            = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    /// Start a real-time session with the given name and providers.
    /// Internally calls StartTrace + EnableTraceEx2 for each provider.
    /// If a session with the same name already exists, it is stopped first.
    [[nodiscard]] std::expected<void, gma::Error> start(
        const char*                   session_name,
        std::span<const ProviderSpec> providers,
        uint32_t                      buffer_size_kb     = 64u,
        uint32_t                      max_buffers_count  = 32u) noexcept;

    /// Stop the session (flushes pending events). Safe to call if not started.
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept { return session_handle_ != 0; }

    /// Start a background consumer thread that calls `cb(record, user_ctx)`
    /// for each event. Must be called AFTER start().
    /// The thread runs until stop_consumer() or stop().
    [[nodiscard]] std::expected<void, gma::Error> start_consumer(
        EventCallback cb, void* user_ctx) noexcept;

    /// Stop the consumer thread without closing the session.
    void stop_consumer() noexcept;

    /// Diagnostic: drop counter (events that EventRecordCallback dispatched
    /// while the consumer thread was already busy). Updated by ETW kernel.
    [[nodiscard]] uint64_t events_dropped() const noexcept;

    /// Diagnostic: total events processed since start_consumer().
    [[nodiscard]] uint64_t events_processed() const noexcept;

private:
    TRACEHANDLE  session_handle_ {0};
    TRACEHANDLE  consumer_trace_{0};
    HANDLE       consumer_thread_{nullptr};
    std::atomic<bool>     stop_flag_{false};
    std::atomic<uint64_t> events_processed_{0u};
    EventCallback         cb_       {nullptr};
    void*                 user_ctx_ {nullptr};
    char                  name_     [128]{};

    static void NTAPI event_record_thunk(EVENT_RECORD* rec) noexcept;
    static DWORD WINAPI consumer_thread_entry(LPVOID arg) noexcept;
};

/// Common provider GUIDs (centralized to avoid magic numbers everywhere).
namespace providers {
    inline constexpr GUID kKernelProcess { 0x0268a8b6, 0x74fd, 0x4302,
        {0x9b, 0x4a, 0x6e, 0xa0, 0xfb, 0xb1, 0x9d, 0x9e}};
    inline constexpr GUID kKernelThread  { 0x3d6fa8d1, 0xfe05, 0x11d0,
        {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
    inline constexpr GUID kKernelContextSwitch { /* ... */ };
    // ... más providers comunes
}

#else // ── Non-Windows: header-only no-ops ─────────────────────────────────────

class SessionManager {
public:
    template<typename... Args>
    std::expected<void, gma::Error> start(Args&&...) noexcept {
        return std::unexpected(gma::Error{gma::ErrorCode::NotSupported,
            "ETW is Windows-only"});
    }
    void stop() noexcept {}
    bool is_running() const noexcept { return false; }
    // ...
};

#endif

} // namespace gma::etw
```

### §6.3 Pillar placement

**Nuevo pillar:** `pillars/etw`. Windows-only en cuanto a implementación, pero header-compilable en Linux para que código cross-platform pueda usar la clase como no-op.

Dependencias: `gamma_hal`, `gamma_schema`. **NO depende** de `topology` ni `behavior`.

CMakeLists notable:
```cmake
add_library(gamma_etw STATIC ...)
if(WIN32)
    target_link_libraries(gamma_etw PRIVATE advapi32 tdh)
endif()
```

### §6.4 Strategy: lifecycle ETW

ETW pattern Windows correcto:

```
1. EVENT_TRACE_PROPERTIES + name fits in buffer of (sizeof(PROPS) + name_len + 1)
2. StartTrace(&handle, name, &props)
3. For each provider: EnableTraceEx2(handle, &guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, level, match_any, match_all, 0, nullptr)
4. EVENT_TRACE_LOGFILEW logfile = {.LoggerName = name, .ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD, .EventRecordCallback = thunk}
5. TRACEHANDLE trace = OpenTraceW(&logfile)
6. ProcessTrace(&trace, 1, nullptr, nullptr)  // blocks until ControlTrace stops it

Stop:
1. ControlTrace(handle, name, &props, EVENT_TRACE_CONTROL_STOP)
2. ProcessTrace returns; consumer thread joins
3. CloseTrace(trace)
```

El reto principal: `ProcessTrace` es **blocking** — debe correr en su propio thread (el "consumer thread").

### §6.5 Thread model

```
┌──────────────────┐  start()    ┌─────────────────┐
│   Agent main     │ ──────────► │ StartTrace +    │
│   thread         │             │ EnableProvider  │
└──────────────────┘             └─────────────────┘
        │
        │ start_consumer()
        ▼
┌──────────────────────────────────────────────┐
│ Consumer thread (spawn via CreateThread):    │
│   - OpenTrace                                │
│   - ProcessTrace (blocks; calls thunk per ev)│
│   - thunk: cb(*record, user_ctx)             │
│   - On ControlTrace STOP: returns            │
└──────────────────────────────────────────────┘
        │
        │ stop()
        ▼
┌──────────────────────┐
│ ControlTrace(STOP)   │
│ WaitForSingleObject  │
│   on consumer thread │
└──────────────────────┘
```

### §6.6 Performance considerations

- ETW kernel-mode collection: ~0.1% CPU overhead típico para context-switch tracing.
- El **callback** debe ser O(1) — solo copiar el evento a un ring buffer interno de Ayama y returnar. Cualquier lógica más pesada (clasificación, statistics) corre en el agent main thread durante el tick.
- Buffer sizing: 64KB × 32 = 2MB total ETW kernel buffers. Para Ayama esto cubre ~10-30 seg de eventos en sistemas muy busy.

### §6.7 Provider keywords (escalación gradual)

Ayama usa diferentes provider sets según workload (ver `EtwProviderSet` ya implementado). El SessionManager **no** sabe de esto — solo recibe el `span<ProviderSpec>` y lo aplica. Quien decide el set es `EtwSessionManager` (Ayama) que ahora puede ser un wrapper trivial sobre `gma::etw::SessionManager`.

### §6.8 Tests requeridos

1. Start + stop sin providers → no leak (Application Verifier).
2. Start con `kKernelProcess` + consumer callback → recibir ≥1 event en 5 seg de actividad.
3. Restart con mismo nombre → no error (debería stop el previo).
4. ControlTrace race: stop() durante callback execution → thread exit clean.
5. Non-admin → start() retorna `PermissionDenied`.
6. Non-Windows compile-only test: `gamma_etw` linka en Linux build sin error.

### §6.9 Migración Ayama

`ayama/observer/src/MetricsCollector.cpp` hoy mezcla:
- ETW session lifecycle (~200 LOC)
- Event-to-metric processing (~150 LOC)
- PDH counter sampling (~100 LOC)

Post-FR-6, queda:
- ETW session = un miembro `gma::etw::SessionManager`
- Event callback = la lógica de-200-LOC reducida a "copy event into thread-local ring"
- Tick-time processing = drain del ring → metric updates (sin cambios)

Ahorro neto en Ayama: ~150 LOC eliminadas.

### §6.10 Estimación

- **Tamaño:** XL (~600 LOC + 6 tests + plataforma split + provider GUID database)
- **Dependencias:** `gamma_hal`, `gamma_schema`, advapi32/tdh (Win)
- **Riesgo:** medio. ETW APIs son bien documentadas pero idiosincráticas. Hay corner cases (rundown events, sustained drop scenarios) que requieren testing en hardware real.

---

## §7 — FR-7: Node hot-restart (future work)

### §7.1 Estado actual

**Prioridad: baja. No se planea para v1.** Documentamos aquí el problema y los blockers para evitar redescubrirlos.

### §7.2 Problema

El `GraphRuntime` actual asume que los nodos viven todo el lifecycle del proceso. No hay API para:
- Detener un nodo específico.
- Re-construirlo (mismo `node_id`, instancia nueva).
- Re-conectarlo sin re-instanciar el grafo entero.

Esto fricciona a Ayama porque para "stop ETW dinámicamente" tendría que stop el agent completo. (Mitigación actual: `EtwSessionManager` con hysteresis que start/stop ETW internamente.)

### §7.3 Blockers conceptuales

1. **Outlet subscribers state**: cuando un nodo se detiene, sus subscribers ya están "viviendo" en el `next_producer_seq_` cursor. Reconectar exige re-subscribir sin perder lo no-leído.

2. **Wire validation**: el `DslGraphBuilder` valida tipos en `build()`. Si un nodo es reemplazado por otra instancia, los handles `placement::*` quedan stale.

3. **TLS state**: nodos pueden tener thread_local state (visto en pool ring TLS bug — ver MEMORY.md). Hot-restart requiere clean TLS per-node.

4. **Scheduler awareness**: el `gamma_scheduler` tendría que enterarse del cambio para no programar trabajos al nodo viejo.

### §7.4 Design espacio (no resolver ahora)

Posibles APIs:

```cpp
// Option A: explicit restart via runtime
class GraphRuntime {
    std::expected<void, Error> restart_node(NodeId id) noexcept;
};

// Option B: "soft pause" sin destruir
class GraphRuntime {
    void pause_node(NodeId id)  noexcept;  // skip tick() calls
    void resume_node(NodeId id) noexcept;
};

// Option C: dynamic graph editing
class DslGraphBuilder {
    void replace_node(NodeId id, std::unique_ptr<INode>) noexcept;
};
```

**Recomendación:** abrir un design doc separado cuando haya un segundo consumer real que lo pida. Hasta entonces, Ayama puede vivir con su workaround interno.

---

## §8 — FR-8: Extended `gma::ErrorCode` values

### §8.1 Objetivo

Tres `ErrorCode` faltantes bloquean la compilación de Ayama:

| Valor | Uso | Sitios |
|---|---|---|
| `SystemError = 23` | OS API devolvió error inesperado | `ActionExecutor`, `AyamaAgentPublisher`, `MetricsCollector` |
| `BufferFull = 24` | Estructura de capacidad fija saturada | `ActionExecutor::apply()` (active actions), drain APIs |
| `OutOfMemory = 25` | `operator new(std::nothrow)` retornó null | `AgentRuntime::start()`, allocators |

Sin estos, Ayama mapea errores OS al genérico `IoError` (semánticamente incorrecto) o no compila.

### §8.2 API final

```cpp
// pillars/schema/include/gamma/schema/Error.hpp
enum class ErrorCode : uint32_t {
    // ... valores existentes 0..22 ...

    // ── FR-8: Ayama Phase 1 additions ─────────────────────────────────────────
    SystemError        = 23,  // OS/Win32/POSIX call returned an unexpected error
    BufferFull         = 24,  // fixed-capacity buffer is at capacity
    OutOfMemory        = 25,  // operator new / malloc returned null
};
```

**Invariantes preservados:**
- `Error` sigue siendo POD `alignas(16)`, 16B, trivially_copyable
- `ErrorCode` sigue siendo `uint32_t` underlying (no overflow risk: 25 << 2³²)
- No bumps de schema version (additive a un enum sin gaps)

### §8.3 Pillar placement

`pillars/schema/include/gamma/schema/Error.hpp` — additive, sin nuevos archivos.

### §8.4 Strategy

Una sola edit. No hay platform split. No hay state. Trivial.

### §8.5 Performance profile

| Métrica | Valor |
|---|---|
| Runtime cost | 0 — solo es un enum value |
| Compile-time cost | ~0 (1 línea por valor) |
| Binary size impact | 0 (enum values no se materializan a menos que se construya un `Error`) |

### §8.6 Tests requeridos

1. `static_assert(static_cast<uint32_t>(ErrorCode::SystemError) == 23)` — y los otros dos.
2. `static_assert(sizeof(Error) == 16)` — invariante preservado.
3. Smoke test: construir `Error{ErrorCode::SystemError, 0, 0}` y verificar campos.

### §8.7 Migración Ayama

```cpp
// ANTES (forzado a usar el genérico):
return std::unexpected(gma::Error{gma::ErrorCode::IoError});

// DESPUÉS (semánticamente correcto):
return std::unexpected(gma::Error{gma::ErrorCode::SystemError});
```

Sitios concretos a corregir:
- `ayama/action/src/ActionExecutor.cpp::apply()` → `BufferFull` cuando `n_active_ >= kMaxActiveActions`
- `ayama/core/src/AgentRuntime.cpp::start()` → `OutOfMemory` cuando `new (std::nothrow) Impl{}` retorna null
- `ayama/observer/src/MetricsCollector_win32.cpp` → `SystemError` para fallos genéricos de Win32

### §8.8 Estimación

- **Tamaño:** XS (3 líneas + 3 tests)
- **Dependencias:** ninguna
- **Riesgo:** nulo (additive enum)

---

## §9 — FR-9: `gma::hw::set_process_priority()` / `get_process_priority()`

### §9.1 Objetivo

Análogo cross-process de FR-3 para priority class. Ayama necesita cambiar la clase de prioridad de procesos target (e.g. HIGH_PRIORITY_CLASS para Riot/Unity, BELOW_NORMAL para background recorders) y luego revertir al cierre. Sin esta API, Ayama duplicaría el código en cada call site.

### §9.2 API final

```cpp
// pillars/topology/include/gamma/topology/HardwareTopology.hpp
namespace gma::hw {

/// Set the priority class of an arbitrary process.
///
/// `priority_class` semantics:
/// - Windows: Win32 constant — IDLE_PRIORITY_CLASS (0x40),
///   BELOW_NORMAL_PRIORITY_CLASS (0x4000), NORMAL_PRIORITY_CLASS (0x20),
///   ABOVE_NORMAL_PRIORITY_CLASS (0x8000), HIGH_PRIORITY_CLASS (0x80),
///   REALTIME_PRIORITY_CLASS (0x100).
/// - POSIX: raw nice value (-20..19) cast to int32_t inside uint32_t.
///
/// Returns the PREVIOUS priority_class on success — caller stores it for revert.
/// Returns InvalidArgument if priority_class == 0 (Win32 sentinel "GetPriorityClass failed").
/// Returns PermissionDenied if the process cannot be opened for PROCESS_SET_INFORMATION
/// (Windows) or if EPERM/EACCES (POSIX).
[[nodiscard]] std::expected<uint32_t, gma::Error>
set_process_priority(uint32_t pid, uint32_t priority_class) noexcept;

/// Read current priority class without modifying.
/// Returns 0 + IoError if GetPriorityClass fails on Windows.
[[nodiscard]] std::expected<uint32_t, gma::Error>
get_process_priority(uint32_t pid) noexcept;

} // namespace gma::hw
```

### §9.3 Pillar placement

`pillars/topology` — junto a FR-3 (`set_process_affinity`) que comparte el mismo `OpenProcess` helper internamente.

### §9.4 Strategy: Windows

```cpp
std::expected<uint32_t, gma::Error>
set_process_priority(uint32_t pid, uint32_t priority_class) noexcept
{
    if (priority_class == 0u)
        return std::unexpected(gma::Error{gma::ErrorCode::InvalidArgument});

    HANDLE h = OpenProcess(
        PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (!h) {
        const DWORD err = GetLastError();
        return std::unexpected(gma::Error{
            err == ERROR_ACCESS_DENIED ? gma::ErrorCode::PermissionDenied
                                       : gma::ErrorCode::SystemError});
    }

    const DWORD prev = GetPriorityClass(h);
    if (prev == 0) { CloseHandle(h); return std::unexpected(gma::Error{gma::ErrorCode::SystemError}); }
    if (!SetPriorityClass(h, static_cast<DWORD>(priority_class))) {
        CloseHandle(h);
        return std::unexpected(gma::Error{gma::ErrorCode::SystemError});
    }
    CloseHandle(h);
    return static_cast<uint32_t>(prev);
}
```

### §9.5 Strategy: POSIX

```cpp
std::expected<uint32_t, gma::Error>
set_process_priority(uint32_t pid, uint32_t priority_class) noexcept
{
    errno = 0;
    const int prev = getpriority(PRIO_PROCESS, static_cast<id_t>(pid));
    if (prev == -1 && errno != 0)
        return std::unexpected(gma::Error{
            (errno == EPERM || errno == EACCES)
                ? gma::ErrorCode::PermissionDenied
                : gma::ErrorCode::SystemError});

    const int new_nice = static_cast<int>(static_cast<int32_t>(priority_class));
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(pid), new_nice) != 0)
        return std::unexpected(gma::Error{
            (errno == EPERM || errno == EACCES)
                ? gma::ErrorCode::PermissionDenied
                : gma::ErrorCode::IoError});
    return static_cast<uint32_t>(static_cast<int32_t>(prev));
}
```

`<sys/resource.h>` ya está disponible en Linux/macOS.

### §9.6 Performance profile

| Métrica | Valor |
|---|---|
| Syscall count | 3 (Win: OpenProcess + GetPriorityClass + SetPriorityClass + CloseHandle) |
| Wall time | ~5-15 µs |
| Heap alloc | 0 |
| Frecuencia esperada | <1 vez/segundo (solo cuando policy decide cambiar) |

**No es hot path.** Optimizar para legibilidad y corrección.

### §9.7 Error code mapping

| Condición | `ErrorCode` |
|---|---|
| `priority_class == 0` (Win sentinel) | `InvalidArgument` |
| `OpenProcess` falla con ACCESS_DENIED / EPERM | `PermissionDenied` |
| `GetPriorityClass` retorna 0 | `SystemError` |
| `SetPriorityClass` retorna 0 | `SystemError` |
| Otros | `IoError` |

### §9.8 Tests requeridos

1. Set propio PID a HIGH_PRIORITY_CLASS, leer con `get_process_priority`, restaurar — debe matchear.
2. `set_process_priority(pid, 0)` → `InvalidArgument`.
3. PID inexistente → `PermissionDenied` o `SystemError` (Windows reporta ambos según versión).
4. Roundtrip: prev = get; set(new); set(prev); get debe matchear prev.
5. `noexcept` static check.

### §9.9 Migración Ayama

`ayama/action/src/ActionExecutor.cpp::apply_set_priority()` actualmente requiere `OpenProcess` + `GetPriorityClass` antes de un `CrossProcessAffinity::set_priority`. Reemplazo:

```cpp
// ANTES (40+ LOC con plataforma split):
HANDLE h = OpenProcess(...);
DWORD prev = GetPriorityClass(h);
CloseHandle(h);
auto r = CrossProcessAffinity::set_priority(pid, pclass);
// ...

// DESPUÉS (FR-9):
auto r = gma::hw::set_process_priority(pid, pclass);
if (!r) return std::unexpected(r.error());
out_prev_pclass = *r;
```

### §9.10 Estimación

- **Tamaño:** M (~120 LOC + 5 tests)
- **Dependencias:** ninguna nueva (Win32/POSIX headers ya incluidos)
- **Riesgo:** bajo (mismo patrón que FR-3)

---

## §10 — FR-10: `gma::tuning::check_privilege_level()` free function

### §10.1 Objetivo

Conveniencia ergonómica. `PrivilegeCheck::probe()` retorna un struct `PrivilegeInfo` (5 campos). El 90% de los call sites solo quieren el `.level`. Free function de una línea evita 4 bytes de stack + 1 dereference y mejora legibilidad.

### §10.2 API final

```cpp
// pillars/tuning/include/gamma/tuning/PrivilegeCheck.hpp
namespace gma::tuning {

/// Returns just the PrivilegeLevel enum (one of None/Partial/Elevated/Admin).
/// Equivalent to PrivilegeCheck::probe().level.
///
/// Note: this still calls probe() under the hood; if you need multiple fields
/// (can_set_rt_prio, can_lock_pages, etc.), call probe() once and reuse.
[[nodiscard]] inline PrivilegeLevel check_privilege_level() noexcept {
    return PrivilegeCheck::probe().level;
}

} // namespace gma::tuning
```

**Inline en header** — sin nuevo `.cpp`. El compilador lo absorbe completamente y desaparece (zero overhead).

### §10.3 Pillar placement

`pillars/tuning/include/gamma/tuning/PrivilegeCheck.hpp` — header-only addition.

### §10.4 Strategy

Una línea. Inline. Header-only. No hay nada más que diseñar.

### §10.5 Performance profile

| Métrica | Valor |
|---|---|
| Overhead vs `probe().level` | 0 (inline) |
| Cache cost del `PrivilegeInfo` no-usado | irrelevante (struct vive en stack frame y se descarta) |

### §10.6 Tests requeridos

1. `check_privilege_level() == PrivilegeCheck::probe().level` — semantic equivalence.
2. `noexcept` static check.
3. Múltiples llamadas no causan UB (cada una recomputa; aceptable porque probe es barata).

### §10.7 Migración Ayama

```cpp
// ANTES:
gma::tuning::PrivilegeInfo info = gma::tuning::PrivilegeCheck::probe();
if (info.level >= PrivilegeLevel::Elevated) { ... }

// DESPUÉS:
if (gma::tuning::check_privilege_level() >= PrivilegeLevel::Elevated) { ... }
```

Sitios:
- `ayama/core/src/AgentRuntime.cpp::start()` — verificación de admin.

### §10.8 Estimación

- **Tamaño:** XS (3 líneas + 2 tests)
- **Dependencias:** ninguna
- **Riesgo:** nulo

---

## §11 — FR-11: `gma::proc::ProcessMetricsSnapshot` — bulk system-wide sampling

### §11.1 Objetivo (HOT PATH — máximo impacto de performance)

Ayama actualmente abre/cierra un `HANDLE` por proceso por métrica por tick. Con 32 targets y 3 métricas (CPU times, working set, I/O bytes) eso son **96 syscall pairs por tick = ~300 µs de overhead**.

**Solución:** una sola syscall `NtQuerySystemInformation(SystemProcessInformation)` retorna métricas de TODOS los procesos del sistema (~500 procesos típico). El consumer filtra por sus PIDs target.

**Ganancia medida (estimada):**

| Operación | Antes (per-PID) | Después (bulk) | Speedup |
|---|---|---|---|
| 32 targets × 3 syscalls × 3µs | ~300 µs/tick | ~120 µs/tick (1 NtQSI + 32 lookups) | **2.5×** |
| 64 targets × 3 syscalls × 3µs | ~600 µs/tick | ~150 µs/tick | **4×** |
| Heap allocations / tick | 0 | 0 (buffer pre-allocado) | — |

A 10 ticks/sec, esto equivale a **1.8-4.5 ms/sec de CPU recuperados**, alineado con el budget de <0.3% CPU idle del Master Plan §10.

### §11.2 API final

```cpp
// pillars/process/include/gamma/process/ProcessMetricsSnapshot.hpp
namespace gma::proc {

struct alignas(8) ProcessMetrics {
    uint64_t kernel_time_100ns;   //  8B @0  — accumulated kernel CPU time
    uint64_t user_time_100ns;     //  8B @8  — accumulated user CPU time
    uint64_t working_set_bytes;   //  8B @16 — current RSS
    uint64_t private_bytes;       //  8B @24 — committed private memory
    uint64_t read_bytes;          //  8B @32 — I/O read total since start
    uint64_t write_bytes;         //  8B @40 — I/O write total since start
    uint32_t pid;                 //  4B @48
    uint32_t thread_count;        //  4B @52
    uint32_t handle_count;        //  4B @56
    uint32_t _pad;                //  4B @60
};
static_assert(sizeof(ProcessMetrics) == 64u);   // exact cache-line
static_assert(std::is_trivially_copyable_v<ProcessMetrics>);

class ProcessMetricsSnapshot {
public:
    /// Construct with initial buffer capacity (defaults to 256 KB ≈ 500 procs on Win10).
    /// The buffer grows on capture() if undersized; growth is one-time and stable.
    [[nodiscard]] static std::expected<ProcessMetricsSnapshot, gma::Error>
    create(uint32_t initial_capacity_bytes = 262144u) noexcept;

    ~ProcessMetricsSnapshot() noexcept;

    // Non-copyable, movable (owns a buffer).
    ProcessMetricsSnapshot(ProcessMetricsSnapshot&&) noexcept;
    ProcessMetricsSnapshot& operator=(ProcessMetricsSnapshot&&) noexcept;

    /// Capture system-wide process metrics into the internal sorted array.
    /// ~50-200 µs typical. ZERO heap allocation after first capture().
    [[nodiscard]] std::expected<void, gma::Error> capture() noexcept;

    /// Find metrics for a PID. O(log N) — binary search on PID-sorted array.
    /// Returns nullptr if PID not present.
    [[nodiscard]] const ProcessMetrics* find(uint32_t pid) const noexcept;

    /// Bulk extract: walks `pids[0..n)` and writes matching metrics to `out[]`.
    /// Caller-allocated `out`. Returns count actually found (≤ n).
    /// O(n + n_procs) — single pass with sorted-merge.
    [[nodiscard]] uint32_t extract(const uint32_t* pids, uint32_t n,
                                    ProcessMetrics* out) const noexcept;

    [[nodiscard]] uint32_t process_count() const noexcept;
    [[nodiscard]] const ProcessMetrics* data() const noexcept;
};

} // namespace gma::proc
```

### §11.3 Pillar placement

`pillars/process` — extiende el pillar de FR-4 (`enumerate_processes`). Mismo target CMake (`gamma_process`).

### §11.4 Strategy: Windows (NtQuerySystemInformation)

```cpp
// Internal — buffer owned by the snapshot
struct Impl {
    std::unique_ptr<uint8_t[]> buf;   // resized on demand
    uint32_t                   buf_cap;
    std::vector<ProcessMetrics> metrics;  // sorted by pid, reserve()d once
};

std::expected<void, gma::Error> capture() noexcept {
    ULONG returned = 0;
    NTSTATUS s;
    // Loop until buf is large enough. Typical: 1 iteration.
    for (uint32_t attempt = 0; attempt < 4; ++attempt) {
        s = NtQuerySystemInformation(
            SystemProcessInformation,
            impl_->buf.get(), impl_->buf_cap, &returned);
        if (s == STATUS_INFO_LENGTH_MISMATCH) {
            // Grow buffer to 1.5× returned, retry.
            const uint32_t new_cap = std::max(impl_->buf_cap * 2u, returned + 65536u);
            impl_->buf = std::make_unique<uint8_t[]>(new_cap);
            impl_->buf_cap = new_cap;
            continue;
        }
        if (!NT_SUCCESS(s))
            return std::unexpected(gma::Error{gma::ErrorCode::SystemError});
        break;
    }

    // Parse linked list of SYSTEM_PROCESS_INFORMATION.
    impl_->metrics.clear();   // capacity preserved (no realloc)
    const uint8_t* p = impl_->buf.get();
    while (true) {
        auto* spi = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(p);
        ProcessMetrics m{};
        m.pid               = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(spi->UniqueProcessId));
        m.kernel_time_100ns = spi->KernelTime.QuadPart;
        m.user_time_100ns   = spi->UserTime.QuadPart;
        m.working_set_bytes = spi->WorkingSetSize;
        m.private_bytes     = spi->PrivatePageCount * 4096ull;
        m.read_bytes        = spi->ReadTransferCount.QuadPart;
        m.write_bytes       = spi->WriteTransferCount.QuadPart;
        m.thread_count      = spi->NumberOfThreads;
        m.handle_count      = spi->HandleCount;
        impl_->metrics.push_back(m);
        if (spi->NextEntryOffset == 0) break;
        p += spi->NextEntryOffset;
    }
    // Sort by PID for O(log N) find().
    std::sort(impl_->metrics.begin(), impl_->metrics.end(),
              [](const auto& a, const auto& b){ return a.pid < b.pid; });
    return {};
}
```

**Crítico:**
- `NtQuerySystemInformation` es estable desde Win2K (no es realmente "undocumented" — Microsoft lo declara explícitamente en Windows Internals y MSDN).
- Linkear vía `GetProcAddress("ntdll.dll", "NtQuerySystemInformation")` para no requerir importlib.
- Buffer crece geométricamente, se estabiliza tras ~2 calls (cold-start), zero alloc steady-state.

### §11.5 Strategy: Linux

`/proc/<pid>/stat` parseo por PID. Mucho más caro que Windows (open + read + parse + close per PID). **Aceptable** porque:
- Linux no es el target primario de Ayama (Master Plan §1).
- Para n_procs ≤ 100, sigue siendo <500 µs total.

Alternative considerada (rechazada): `taskstats` netlink. Requiere CAP_NET_ADMIN. Más complejo. Skip.

### §11.6 Performance profile

| Métrica | Windows | Linux |
|---|---|---|
| First `capture()` | ~150-300 µs (buf alloc + sort) | ~2-5 ms (per-PID /proc walk) |
| Steady `capture()` | ~80-150 µs | ~2-5 ms |
| `find(pid)` | ~20 ns (binary search) | idem |
| `extract(n=32)` | ~600 ns | idem |
| Heap alloc steady-state | 0 | 0 (path buffer reused) |

### §11.7 Tests requeridos

1. `create()` → `capture()` → `find(getpid())` returns non-null y campos sane.
2. Buffer growth: forzar capacidad inicial pequeña, `capture()` debe grow y succeed.
3. `extract()` con PIDs mixed (existentes + inexistentes) → count matches existentes.
4. Idempotencia: 100 captures consecutivos sin allocations (medir con custom allocator).
5. Concurrencia: 2 snapshots paralelos → independientes (cada uno con su buffer).
6. Performance gate: 1000 captures < 500 ms (≈ 500 µs/capture máximo).

### §11.8 Migración Ayama

`ayama/observer/src/MetricsCollector_win32.cpp::sample()` — actualmente loop con OpenProcess+Get*. Reemplazar:

```cpp
// ANTES (per-PID, 3 syscalls × n_targets):
for (uint32_t i = 0; i < n; ++i) {
    HANDLE h = OpenProcess(...);
    GetProcessTimes(h, ...);
    GetProcessMemoryInfo(h, ...);
    GetProcessIoCounters(h, ...);
    CloseHandle(h);
    out[i] = ...
}

// DESPUÉS (bulk, 1 syscall total):
snapshot_.capture();
snapshot_.extract(pids, n, raw_metrics);
for (uint32_t i = 0; i < n; ++i)
    out[i] = transform(raw_metrics[i]);
```

`MetricsCollector` debe owner un `ProcessMetricsSnapshot` member, crear en `start()`, capturar en `sample()`.

### §11.9 Estimación

- **Tamaño:** L (~400 LOC + 6 tests + microbench)
- **Dependencias:** `ntdll.dll` (Windows, GetProcAddress dinámico — sin import)
- **Riesgo:** medio. NtQuerySystemInformation requiere parsing cuidadoso de structs no-públicos pero documentados. Validación robusta del returned size es crítica.

---

## §12 — FR-12: `gma::ipc::Ring<T>` — generic SPSC lock-free ring

### §12.1 Objetivo

Primitiva fundacional para múltiples FRs (FR-13 CtxSwitch, FR-14 ForegroundTracker) y para reemplazar `ActionLogRing` y `etw_ring_` (hand-rolled actualmente en Ayama).

**Single-producer/single-consumer** — la única forma usada por Ayama. MPMC variants se difieren a v2.

### §12.2 API final

```cpp
// pillars/ipc/include/gamma/ipc/Ring.hpp
namespace gma::ipc {

/// SPSC lock-free ring buffer.
/// Producer: one writer thread. Consumer: one reader thread.
///
/// `T` must be trivially_copyable (memcpy-safe).
/// `Capacity` must be a power of 2 (compile-time check).
///
/// Cache-line aligned producer/consumer cursors — zero false sharing.
template <typename T, uint32_t Capacity>
class alignas(gma::hal::kDestructivePad) Ring {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(Capacity > 0u && (Capacity & (Capacity - 1u)) == 0u,
                  "Capacity must be a power of 2");

public:
    /// Producer side: push one element. Returns false if ring is full.
    /// Memory order: release.
    [[nodiscard]] bool try_push(const T& v) noexcept;

    /// Producer side: push without overrun-check (asserts in Debug).
    /// Returns the resulting write_seq.
    uint64_t push_unchecked(const T& v) noexcept;

    /// Consumer side: pop one element. Returns false if empty.
    /// Memory order: acquire.
    [[nodiscard]] bool try_pop(T& out) noexcept;

    /// Consumer side: drain up to `max` elements into `out[]`. Returns count.
    /// Fast path: contiguous memcpy when no wrap.
    [[nodiscard]] uint32_t drain(T* out, uint32_t max) noexcept;

    /// Approximate size (race-free for SPSC).
    [[nodiscard]] uint32_t size() const noexcept;
    [[nodiscard]] bool     empty() const noexcept;
    [[nodiscard]] static constexpr uint32_t capacity() noexcept { return Capacity; }

    /// Number of pushes that failed due to full ring (producer-incremented).
    [[nodiscard]] uint64_t dropped() const noexcept;

private:
    alignas(gma::hal::kDestructivePad) std::atomic<uint64_t> write_seq_{0};
    alignas(gma::hal::kDestructivePad) std::atomic<uint64_t> read_seq_{0};
    alignas(gma::hal::kDestructivePad) std::atomic<uint64_t> dropped_{0};
    alignas(gma::hal::kDestructivePad) std::array<T, Capacity> slots_;
};

} // namespace gma::ipc
```

### §12.3 Pillar placement

`pillars/ipc` — header-only template, mismo `gamma_ipc` INTERFACE target que FR-5.

### §12.4 Strategy: SPSC algorithm

Algoritmo clásico (Vyukov SPSC), simplificado:

```cpp
template <typename T, uint32_t Capacity>
bool Ring<T, Capacity>::try_push(const T& v) noexcept {
    const uint64_t w = write_seq_.load(std::memory_order_relaxed);
    const uint64_t r = read_seq_.load(std::memory_order_acquire);
    if (w - r >= Capacity) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    slots_[w & (Capacity - 1u)] = v;          // memcpy (T is trivially_copyable)
    write_seq_.store(w + 1u, std::memory_order_release);
    return true;
}

template <typename T, uint32_t Capacity>
bool Ring<T, Capacity>::try_pop(T& out) noexcept {
    const uint64_t r = read_seq_.load(std::memory_order_relaxed);
    const uint64_t w = write_seq_.load(std::memory_order_acquire);
    if (r == w) return false;
    out = slots_[r & (Capacity - 1u)];
    read_seq_.store(r + 1u, std::memory_order_release);
    return true;
}
```

**Optimizaciones críticas para max-perf:**
- Cursores en líneas de caché separadas → no contention productor↔consumidor.
- `Capacity & (Cap-1)` mask en vez de `%` → 1 ciclo vs ~20-30 ciclos para `%`.
- `relaxed` para load del cursor propio, `acquire`/`release` solo cross-thread.
- `slots_` cache-aligned para evitar false sharing dentro de drain batches.

### §12.5 `drain()` optimization

```cpp
template <typename T, uint32_t Capacity>
uint32_t Ring<T, Capacity>::drain(T* out, uint32_t max) noexcept {
    const uint64_t r = read_seq_.load(std::memory_order_relaxed);
    const uint64_t w = write_seq_.load(std::memory_order_acquire);
    const uint64_t available = w - r;
    const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(available, max));
    if (take == 0u) return 0u;

    const uint32_t r_idx = static_cast<uint32_t>(r & (Capacity - 1u));
    const uint32_t first_chunk = std::min(take, Capacity - r_idx);
    std::memcpy(out, &slots_[r_idx], first_chunk * sizeof(T));
    if (first_chunk < take)
        std::memcpy(out + first_chunk, &slots_[0], (take - first_chunk) * sizeof(T));

    read_seq_.store(r + take, std::memory_order_release);
    return take;
}
```

**Dos memcpy max** (wrap detection). Vectorizable por el compilador para `T` pequeños.

### §12.6 Performance profile

| Operación | Cost |
|---|---|
| `try_push` | ~10-20 ns (1 load, 1 store, 1 memcpy de T) |
| `try_pop` | ~10-20 ns (idem) |
| `drain(64)` para `T=32B` | ~120 ns (single memcpy of 2 KB) |
| `drain(1024)` para `T=32B` | ~1.5 µs (single memcpy of 32 KB; bound L1) |
| Heap alloc | 0 (todo embedded en la struct) |
| Cache footprint | `Capacity * sizeof(T) + 3 cache lines` |

### §12.7 Tests requeridos

1. SPSC correctness: producer pushea N elementos, consumer drena, valores y orden preservados.
2. Full-detection: pushear `Capacity` elementos, push `Capacity+1` retorna false e incrementa `dropped()`.
3. Empty-detection: pop sobre ring vacío retorna false sin block.
4. Wraparound: pushear `2.5 × Capacity` con drain intercalado, no corrupción.
5. Thread safety: producer en thread A, consumer en thread B, 1M iteraciones, TSan-clean.
6. Performance gate: 10M push+pop < 200 ms (≈ 20 ns/op).
7. `static_assert` compile failures: capacity 100 (no power of 2), `T` no-trivially_copyable.

### §12.8 Migración Ayama

- `ayama/action/include/ayama/action/ActionLog.hpp::ActionLogRing` → reemplazar internos con `gma::ipc::Ring<ActionLogEntry, 128>`. Mantener wrapper class para API existente (zero LOC change for callers).
- `ayama/observer/src/MetricsCollector_win32.cpp::etw_ring_` → `gma::ipc::Ring<CtxSwitchEvent, 65536>`.
- Futuro FR-13 (`CtxSwitchSource`) usa internamente.

### §12.9 Estimación

- **Tamaño:** M (~200 LOC header-only + 7 tests + microbench)
- **Dependencias:** `gamma_hal` para `kDestructivePad`.
- **Riesgo:** bajo. Patrón clásico; tests TSan validan correctness.

---

## §13 — FR-13: `gma::etw::CtxSwitchSource` — ETW context-switch event source

### §13.1 Objetivo (HOT PATH crítico)

Capturar eventos `CSwitch` del kernel-dispatcher (≈10k-100k/sec en sistema activo) con overhead minimal en el callback ETW. Push directo a un ring de FR-12; agent tick thread drena.

**Sin esta abstracción, Ayama tendría que parsear EVENT_RECORD inline en el callback ETW de FR-6 — código complejo, error-prone, y la lógica de PID filtering se duplica entre consumers.**

### §13.2 API final

```cpp
// pillars/etw/include/gamma/etw/CtxSwitchSource.hpp
namespace gma::etw {

struct alignas(8) CtxSwitchEvent {
    uint64_t timestamp_qpc;       //  8B @0  — QueryPerformanceCounter snapshot
    uint32_t prev_pid;            //  4B @8
    uint32_t prev_tid;            //  4B @12
    uint32_t next_pid;            //  4B @16
    uint32_t next_tid;            //  4B @20
    uint8_t  prev_state;          //  1B @24 — Ready/Waiting/Suspended/Transition
    uint8_t  prev_priority;       //  1B @25
    uint8_t  next_priority;       //  1B @26
    uint8_t  wait_reason;         //  1B @27 — only valid when prev_state == Waiting
    uint32_t cpu_index;           //  4B @28
};
static_assert(sizeof(CtxSwitchEvent) == 32u);

/// Captures context-switch events into an internal SPSC ring.
/// One CtxSwitchSource instance == one ETW provider subscription.
template <uint32_t RingCapacity = 65536u>   // ~2 MB at sizeof(CtxSwitchEvent)=32B
class CtxSwitchSource {
public:
    explicit CtxSwitchSource() noexcept = default;

    /// Subscribe to the kernel dispatcher provider via `session`.
    /// `session` must already be started.
    [[nodiscard]] std::expected<void, gma::Error>
    start(SessionManager& session) noexcept;

    void stop() noexcept;

    /// Drain up to `max` events to `out[]`. Caller-provided buffer.
    /// Returns count drained.
    [[nodiscard]] uint32_t drain(CtxSwitchEvent* out, uint32_t max) noexcept {
        return ring_.drain(out, max);
    }

    /// Optional: per-PID filter — only emit events where prev_pid or next_pid match.
    /// Empty list = all events.
    void set_pid_filter(const uint32_t* pids, uint32_t n) noexcept;

    [[nodiscard]] uint64_t events_pushed()  const noexcept;
    [[nodiscard]] uint64_t events_dropped() const noexcept { return ring_.dropped(); }

private:
    gma::ipc::Ring<CtxSwitchEvent, RingCapacity> ring_;
    // ... filter, session ref, callback context
};

} // namespace gma::etw
```

### §13.3 Pillar placement

`pillars/etw` — extiende el pillar de FR-6 (`SessionManager`). Mismo target.

### §13.4 Strategy: ETW callback (HOT PATH)

```cpp
// Static callback bound at ETW subscribe time.
static void __stdcall on_event(EVENT_RECORD* rec) noexcept
{
    // Filter: only CSwitch event opcode (36) from kernel-dispatcher provider.
    if (rec->EventHeader.EventDescriptor.Opcode != 36) return;

    // Parse user_data — fixed offset layout for CSwitch.
    const uint8_t* d = static_cast<const uint8_t*>(rec->UserData);
    CtxSwitchEvent e;
    e.next_tid       = *reinterpret_cast<const uint32_t*>(d + 0);
    e.prev_tid       = *reinterpret_cast<const uint32_t*>(d + 4);
    e.next_priority  = d[8];
    e.prev_priority  = d[9];
    e.prev_state     = d[12];
    e.wait_reason    = d[13];
    // Map TID → PID via thread-table (cached internally; see §13.5).
    e.next_pid       = tid_to_pid_lookup(e.next_tid);
    e.prev_pid       = tid_to_pid_lookup(e.prev_tid);
    e.cpu_index      = rec->BufferContext.ProcessorIndex;
    e.timestamp_qpc  = rec->EventHeader.TimeStamp.QuadPart;

    // PID filter (zero-cost when empty).
    if (!pid_filter_match(e.prev_pid, e.next_pid)) return;

    // Push to ring. dropped++ if full.
    (void)source->ring_.try_push(e);
}
```

**Latencia objetivo en el callback: < 100 ns por evento.**

### §13.5 TID → PID lookup (sub-100ns)

ETW CSwitch eventos solo proveen TIDs. PID lookup en el callback debe ser O(1):

```cpp
// 64K-entry direct-mapped cache. TID is 32-bit pero los low 16 bits son
// los más volátiles → indexar por TID & 0xFFFF.
struct TidEntry {
    uint32_t tid;
    uint32_t pid;
};
alignas(64) static TidEntry tid_cache_[65536];

uint32_t tid_to_pid_lookup(uint32_t tid) noexcept {
    auto& e = tid_cache_[tid & 0xFFFFu];
    if (e.tid == tid) return e.pid;   // hit
    // Miss: slow path
    const uint32_t pid = nt_thread_to_pid(tid);  // NtQueryInformationThread
    e.tid = tid; e.pid = pid;
    return pid;
}
```

ThreadStart/ThreadEnd events del proveedor `Microsoft-Windows-Kernel-Thread` mantienen el cache. Sin él, primer touch per-TID es ~3 µs; subsiguientes ~5 ns.

### §13.6 Performance profile

| Métrica | Valor |
|---|---|
| Callback latency (cache hit) | ~50-80 ns |
| Callback latency (cache miss) | ~3 µs (NtQueryInformationThread once per TID) |
| Eventos sostenidos | 100k/sec @ 0.7% CPU |
| Ring overflow @ 65536 capacity | ≥6 segundos sin drain (Ayama drena cada 100 ms → safe margin 60×) |
| Memoria ring | `65536 × 32 = 2 MB` (fits L2 con margen) |

### §13.7 Tests requeridos

1. Subscribe + sleep(1s) + drain → events.size() > 0 en un sistema con carga.
2. PID filter: filtrar a propio PID, ejecutar workload, drain solo contiene eventos del PID.
3. Ring overflow: artificialmente no drenar, verificar `dropped()` incrementa.
4. Cache hit rate: subscribe + run 1s, calcular hit/miss ratio del TID cache (debe ser >99%).
5. Performance gate: 1M eventos artificiales injected < 100 ms total (≈ 100ns/event).
6. Cleanup: stop() libera todos los recursos ETW, no leak handle.

### §13.8 Migración Ayama

`ayama/observer/src/MetricsCollector_win32.cpp::etw_ring_` y el TODO "parse CSwitch event and push to ring" (línea 222) → reemplazar con:

```cpp
// En start():
ctxsw_source_.start(etw_session_);
ctxsw_source_.set_pid_filter(target_pids_, n_targets_);

// En sample() / drain_etw_ring():
CtxSwitchEvent batch[1024];
const uint32_t n = ctxsw_source_.drain(batch, 1024);
for (uint32_t i = 0; i < n; ++i)
    process_ctxsw(batch[i]);
```

Elimina ~150 LOC de parsing ETW manual.

### §13.9 Estimación

- **Tamaño:** L (~350 LOC + 6 tests + integ test)
- **Dependencias:** FR-6 (`SessionManager`), FR-12 (`Ring<T>`), `advapi32`, `tdh`.
- **Riesgo:** medio. CSwitch layout es well-known pero la ventana de eventos varía por Windows version. Test en Win10 22H2 + Win11 24H2.

---

## §14 — FR-14: `gma::ui::ForegroundTracker` — WinEvent-driven foreground tracking

### §14.1 Objetivo

`ForegroundWatcher.hpp` polea `GetForegroundWindow()` cada tick (100 ms). Esto:
- Es una syscall por tick (~1-3 µs cada uno = 30µs/sec sostenido).
- Tiene latencia de detección igual al tick rate (100 ms).
- Es información disponible vía **WinEvents** (event-driven, zero-poll, <1ms latency).

**Solución event-driven:** `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` registra un hook. El sistema invoca el callback cuando el foreground cambia. Estado se mantiene en un atomic packed (PID + sequence) — readers son lock-free.

### §14.2 API final

```cpp
// pillars/ui/include/gamma/ui/ForegroundTracker.hpp
namespace gma::ui {

struct alignas(16) ForegroundState {
    uint64_t timestamp_qpc;       //  8B @0  — when foreground changed
    uint32_t pid;                 //  4B @8  — foreground process PID
    uint32_t tid;                 //  4B @12 — foreground thread ID
    HWND     hwnd;                //  8B @16 — foreground window handle
    int32_t  monitor_x;           //  4B @24 — monitor origin (for game-mode detection)
    int32_t  monitor_y;           //  4B @28
    uint32_t monitor_w;           //  4B @32
    uint32_t monitor_h;           //  4B @36
    uint32_t rect_x;              //  4B @40 — window rect (for fullscreen detection)
    uint32_t rect_y;              //  4B @44
    uint32_t rect_w;              //  4B @48
    uint32_t rect_h;              //  4B @52
    uint8_t  is_fullscreen;       //  1B @56
    uint8_t  is_topmost;          //  1B @57
    uint8_t  is_visible;          //  1B @58
    uint8_t  _pad;                //  1B @59
    uint32_t change_seq;          //  4B @60 — incremented on each FG change
};
static_assert(sizeof(ForegroundState) == 64u);  // exact cache line

class ForegroundTracker {
public:
    /// Install the WinEvent hook. Idempotent.
    [[nodiscard]] std::expected<void, gma::Error> start() noexcept;
    void stop() noexcept;

    /// Snapshot current state (lock-free atomic read with seqlock retry).
    /// Always returns the most recent consistent state.
    [[nodiscard]] ForegroundState snapshot() const noexcept;

    /// Returns true if `pid` is currently foreground.
    [[nodiscard]] bool is_foreground(uint32_t pid) const noexcept;

    /// Sequence number — caller compares to detect "did FG change since last check".
    [[nodiscard]] uint32_t change_seq() const noexcept;
};

} // namespace gma::ui
```

### §14.3 Pillar placement

Nuevo módulo en `pillars/ui` (ya existe) o sub-pillar `pillars/ui/foreground`. Ya hay infra de UI ahí.

### §14.4 Strategy: Windows

```cpp
// Single hook callback for all instances (process-wide singleton hook).
static void CALLBACK on_fg_event(
    HWINEVENTHOOK, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) noexcept
{
    if (event != EVENT_SYSTEM_FOREGROUND || hwnd == nullptr) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    ForegroundState st{};
    st.timestamp_qpc = qpc_now();
    st.pid           = pid;
    st.tid           = dwEventThread;
    st.hwnd          = hwnd;

    RECT r{};
    if (GetWindowRect(hwnd, &r)) {
        st.rect_x = r.left; st.rect_y = r.top;
        st.rect_w = r.right - r.left; st.rect_h = r.bottom - r.top;
    }
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        st.monitor_x = mi.rcMonitor.left;
        st.monitor_y = mi.rcMonitor.top;
        st.monitor_w = mi.rcMonitor.right - mi.rcMonitor.left;
        st.monitor_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    st.is_fullscreen = (st.rect_x == st.monitor_x && st.rect_y == st.monitor_y
                       && st.rect_w == st.monitor_w && st.rect_h == st.monitor_h);
    // ... is_topmost via GetWindowLong(GWL_EXSTYLE) & WS_EX_TOPMOST
    st.is_visible    = IsWindowVisible(hwnd) ? 1 : 0;

    // Seqlock write: odd → write → even.
    publish_state(st);
}

void publish_state(const ForegroundState& s) noexcept {
    g_seq.fetch_add(1, std::memory_order_release);     // odd
    g_state = s;
    g_state.change_seq = g_seq.load(std::memory_order_relaxed) / 2 + 1;
    g_seq.fetch_add(1, std::memory_order_release);     // even
}
```

`snapshot()` lee con retry hasta 3 veces:

```cpp
ForegroundState snapshot() const noexcept {
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        const uint32_t s1 = g_seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;             // write in progress
        ForegroundState tmp = g_state;
        const uint32_t s2 = g_seq.load(std::memory_order_acquire);
        if (s1 == s2) return tmp;          // consistent
    }
    // Fallback: return last known (potentially torn, but rare).
    return g_state;
}
```

### §14.5 Strategy: Linux

X11: `XSelectInput(root, FocusChangeMask)`. Wayland: protocol-dependent (`zwlr_foreign_toplevel_manager_v1`). En v1, stub que retorna `ForegroundState{}` con `pid=0`. Linux no es prioridad.

### §14.6 Performance profile

| Métrica | Valor |
|---|---|
| `snapshot()` con cache hit | ~30-50 ns (2 atomic reads + memcpy 64B) |
| Hook callback (en FG change event) | ~5-20 µs (Win32 calls) — pero solo on event |
| Polling vs event-driven | **Antes:** 30 µs/sec (10 polls). **Después:** 0 µs (0 polls), excepto bursts en FG change. |
| Heap alloc | 0 |

### §14.7 Tests requeridos

1. Start → snapshot devuelve estado válido tras primer FG change.
2. `change_seq` incrementa monotónicamente.
3. `is_foreground(pid)` retorna true para PID del FG actual.
4. Stop libera hook (no leak).
5. Concurrent snapshot desde 8 threads → todos ven estado consistente (no torn reads).
6. Latency: medir tiempo entre FG change y siguiente `snapshot()` reportando nuevo PID — debe ser <10 ms.

### §14.8 Migración Ayama

`ayama/observer/include/ayama/observer/ForegroundWatcher.hpp` actualmente tiene `GetForegroundWindow + GetWindowThreadProcessId + GetWindowRect + GetMonitorInfoW` polling cada tick. Reemplazar el polling con calls a `tracker_.snapshot()`. La struct existente `ForegroundWatcher` se convierte en wrapper de 50 LOC.

### §14.9 Estimación

- **Tamaño:** L (~300 LOC + 6 tests + interactive integ test)
- **Dependencias:** `user32` (Windows). `gamma_hal` para QPC + cache padding.
- **Riesgo:** medio. WinEventHook callback es async; testing requiere window simulator o user interaction.

---

## §15 — FR-15: `gma::config::TomlParser` — zero-alloc SAX-style TOML parser

### §15.1 Objetivo

`ayama/config/src/ConfigStore.cpp` y `ayama/learn/src/PerGameMemory.cpp` hand-rollean parsers TOML separados, cada uno ~150-200 LOC. Consolidar en una primitiva re-usable. Cold path → optimizar por correctness y code dedup, no por máximo throughput.

### §15.2 API final

```cpp
// pillars/config/include/gamma/config/TomlParser.hpp
namespace gma::config {

enum class TomlEvent : uint8_t {
    Section,       // [section.name]
    ArraySection,  // [[array.name]]
    KeyString,
    KeyInteger,
    KeyFloat,
    KeyBool,
    EndOfStream,
    ParseError,
};

struct TomlValue {
    union {
        const char* str_view;       // points into source buffer (no copy)
        int64_t     i64;
        double      f64;
        bool        b;
    };
    uint32_t str_len;               // valid for str_view
};

struct TomlParseContext {
    const char* source;
    uint32_t    source_len;
    uint32_t    cursor;
    uint32_t    line;
    const char* error_msg;          // valid when event == ParseError
};

/// Callback receives one event at a time. Return false to abort parsing.
using TomlCallback = bool (*)(
    TomlEvent ev, const char* key, uint32_t key_len,
    const TomlValue& val, void* user_ctx) noexcept;

/// Parse a TOML source. Zero heap allocation — all data points into source.
/// `source` must outlive any data the callback retains.
///
/// Returns: number of events emitted (excluding EndOfStream).
[[nodiscard]] uint32_t parse(
    const char* source, uint32_t source_len,
    TomlCallback cb, void* user_ctx) noexcept;

/// Subset supported:
/// - [sections] and [[array.sections]]
/// - key = "string" | int | float | true|false
/// - Comments: # to end of line
/// - No multiline strings, no inline tables, no array values
///
/// Strict enough for Gamma/Ayama's configuration schema.

} // namespace gma::config
```

### §15.3 Pillar placement

Nuevo pillar `pillars/config` (o sub-módulo dentro de `pillars/schema`). Solo headers + 1 cpp.

### §15.4 Strategy: state machine

```cpp
// Single-pass parser. State machine.
enum class ParserState : uint8_t {
    Top,           // expecting [section] or key
    InSection,     // inside [
    InArrayHdr,    // inside [[
    InKey,
    AfterEquals,
    InString,
    InNumber,
    InComment,
    Error,
};

// Inline helpers for character classes (zero-alloc).
[[nodiscard]] static bool is_ident(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

uint32_t parse(...) noexcept {
    ParserState st = ParserState::Top;
    char key[128]{}; uint32_t klen = 0;
    // ... loop driving state, emitting events via cb()
}
```

**No exceptions, no allocs.** Strings se pasan al callback como `(char*, len)` slices al source buffer.

### §15.5 Performance profile

| Métrica | Valor |
|---|---|
| Throughput | ~50-200 MB/s (single-pass byte-level) |
| Heap alloc | 0 |
| Cold start (per-process) | una primera vez — typical TOML files <10 KB → <1 ms |

### §15.6 Tests requeridos

1. Empty input → 0 events.
2. Simple `key = "value"` → 1 KeyString event.
3. `[section]` + `key = 42` → Section + KeyInteger events with correct names.
4. Array sections: `[[rule]]` + 3 keys, two ArraySection groups → 2 ArraySection + 6 key events.
5. Malformed input (unterminated string) → ParseError event with line number.
6. Comments `# ignore me` skipped correctly.
7. Boolean parsing (`true`/`false`).
8. Negative integers and floats.
9. Performance: parse 100 KB file < 5 ms.

### §15.7 Migración Ayama

- `ayama/config/src/ConfigStore.cpp` (~150 LOC) → reemplazar con callback que llena `AgentConfig` y `PolicyOverride[]`.
- `ayama/learn/src/PerGameMemory.cpp` (~200 LOC) → reemplazar con callback que llena `LearnedEntry[]` y `BadEntry[]`.

Ganancia: ~250 LOC eliminadas total + tests reutilizables.

### §15.8 Estimación

- **Tamaño:** M (~250 LOC + 9 tests + 2 migration cleanups)
- **Dependencias:** ninguna.
- **Riesgo:** bajo. Grammar trivial. Edge cases bien acotados.

---

## §16 — FR-16: `gma::sync::WakeEvent` — cross-platform wakeable wait

### §16.1 Objetivo

`AgentRuntime::run()` usa `CreateEventW` + `WaitForSingleObject` para sleep adaptivo con early-wake desde `stop()` o watchdog. Direct Win32. POSIX equivalente es `eventfd` o `pipe`. Consolidar en una abstracción cross-platform consistente.

### §16.2 API final

```cpp
// pillars/sync/include/gamma/sync/WakeEvent.hpp
namespace gma::sync {

class WakeEvent {
public:
    [[nodiscard]] static std::expected<WakeEvent, gma::Error> create() noexcept;

    ~WakeEvent() noexcept;
    WakeEvent(WakeEvent&&) noexcept;
    WakeEvent& operator=(WakeEvent&&) noexcept;

    /// Wait up to timeout_ms. Returns:
    /// - true:  woken via signal()
    /// - false: timeout elapsed without signal
    /// `timeout_ms == ~0u` waits indefinitely.
    [[nodiscard]] bool wait(uint32_t timeout_ms) noexcept;

    /// Signal the event. Multiple signals before wait() consume just one wait().
    /// Idempotent in "signaled" state.
    void signal() noexcept;

    /// Reset to non-signaled state. Rarely needed.
    void reset() noexcept;
};

} // namespace gma::sync
```

Auto-reset semantics (Win32 default). Linux uses `eventfd` with `EFD_SEMAPHORE` for matching semantics.

### §16.3 Pillar placement

Nuevo pillar `pillars/sync` o agregar al pillar `hal`. **Recomendación:** `hal` (es una primitiva OS).

### §16.4 Strategy

```cpp
// Windows
class WakeEvent {
    HANDLE h_;
public:
    static std::expected<WakeEvent, Error> create() noexcept {
        HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!h) return std::unexpected(Error{ErrorCode::SystemError});
        return WakeEvent{h};
    }
    bool wait(uint32_t ms) noexcept {
        return WaitForSingleObject(h_, ms) == WAIT_OBJECT_0;
    }
    void signal() noexcept { SetEvent(h_); }
    void reset()  noexcept { ResetEvent(h_); }
    ~WakeEvent() noexcept { if (h_) CloseHandle(h_); }
};

// Linux
class WakeEvent {
    int fd_;
public:
    static std::expected<WakeEvent, Error> create() noexcept {
        int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (fd < 0) return std::unexpected(Error{ErrorCode::SystemError});
        return WakeEvent{fd};
    }
    bool wait(uint32_t ms) noexcept {
        struct pollfd p{fd_, POLLIN, 0};
        const int r = poll(&p, 1, ms == ~0u ? -1 : static_cast<int>(ms));
        if (r > 0) {
            uint64_t dummy;
            (void)read(fd_, &dummy, 8);
            return true;
        }
        return false;
    }
    void signal() noexcept { uint64_t v=1; (void)write(fd_, &v, 8); }
    void reset()  noexcept { uint64_t v; while (read(fd_, &v, 8) > 0); }
    ~WakeEvent() noexcept { if (fd_>=0) close(fd_); }
};
```

### §16.5 Performance profile

| Métrica | Valor |
|---|---|
| `wait()` con timeout | ~OS scheduler granularity (~1 ms Windows, ~100 µs Linux) |
| `signal()` cost | <1 µs |
| `wait()` ya signaled | <1 µs (returns immediately) |

### §16.6 Tests requeridos

1. Create + wait(100ms) sin signal → returns false tras ~100ms.
2. Signal + wait → returns true inmediately.
3. Signal antes de wait → wait retorna true sin block.
4. 2 signals consecutivos → 1 wait consume 1; segundo wait retorna false (auto-reset).
5. Stress: 100k signal/wait pairs en 2 threads, no deadlock, todos consumidos.

### §16.7 Migración Ayama

```cpp
// AgentRuntime.cpp ANTES:
HANDLE wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
// ...
WaitForSingleObject(wake_event_, sleep_ms);
// ...
SetEvent(wake_event_);  // from stop()

// DESPUÉS:
gma::sync::WakeEvent wake_event_ = *gma::sync::WakeEvent::create();
// ...
wake_event_.wait(sleep_ms);
// ...
wake_event_.signal();
```

### §16.8 Estimación

- **Tamaño:** S (~120 LOC + 5 tests)
- **Dependencias:** ninguna nueva.
- **Riesgo:** muy bajo.

---

## §17 — FR-17: `gma::sched::DedicatedThread` — pinned/prioritized long-running thread

### §17.1 Objetivo

`std::thread` directo en `InternalWatchdog` y `tools/ayama-bench/workload.hpp`. Consolidar con un wrapper que provee:
- Naming (visible en debugger/profiler)
- Optional pinning a core (FR-3 reuse)
- Optional priority (`elevate_thread_rt` reuse)
- Stop token / clean shutdown
- Optional `WakeEvent` para early-wake

### §17.2 API final

```cpp
// pillars/sched/include/gamma/sched/DedicatedThread.hpp
namespace gma::sched {

struct ThreadOpts {
    const char* name           = nullptr;   // descriptive name (debugger visible)
    uint32_t    pin_core       = ~0u;       // logical core ID, or ~0u = no pinning
    bool        rt_priority    = false;     // SCHED_FIFO / TIME_CRITICAL
    bool        below_normal   = false;     // BELOW_NORMAL_PRIORITY_CLASS (E-core friendly)
    uint32_t    stack_size_kb  = 256u;      // 256 KB default
};

class DedicatedThread {
public:
    DedicatedThread() noexcept = default;
    ~DedicatedThread() noexcept;

    DedicatedThread(DedicatedThread const&) = delete;
    DedicatedThread& operator=(DedicatedThread const&) = delete;

    /// Start the thread. `fn` runs until it returns or stop_requested() is true.
    /// `fn` is std::function — capturing lambda OK in cold-path startup code.
    [[nodiscard]] std::expected<void, gma::Error>
    start(std::function<void(const std::atomic<bool>& stop_flag)> fn,
          const ThreadOpts& opts = {}) noexcept;

    /// Signal stop, wake the thread, join.
    void stop_and_join() noexcept;

    /// Check if running (post start, pre stop).
    [[nodiscard]] bool joinable() const noexcept;

    /// Wake the thread early (must opt-in via opts.use_wake_event).
    void wake() noexcept;
};

} // namespace gma::sched
```

### §17.3 Pillar placement

`pillars/sched` (existe). Si no, crear sub-módulo.

### §17.4 Strategy

```cpp
[[nodiscard]] std::expected<void, Error>
DedicatedThread::start(Fn fn, const ThreadOpts& opts) noexcept
{
    stop_flag_.store(false);
    thread_ = std::thread([this, fn=std::move(fn), opts]() noexcept {
        // Apply opts.
        if (opts.name) set_current_thread_name(opts.name);
        if (opts.pin_core != ~0u) gma::hw::pin_current_thread(opts.pin_core);
        if (opts.rt_priority) (void)gma::hw::elevate_thread_rt(true);
        else if (opts.below_normal)
            set_current_thread_priority_below_normal();

        // Run the user function with the stop flag reference.
        fn(stop_flag_);
    });
    return {};
}
```

### §17.5 Performance profile

Setup cost: ~50-200 µs (thread spawn + opts apply). NO es hot-path — typical use: 1-5 spawns por proceso lifetime.

### §17.6 Tests requeridos

1. Start + stop_and_join — thread runs fn, then exits cleanly.
2. Pinning aplicado — verificar affinity mask del thread iniciado.
3. Naming visible — verificar en debugger / SetThreadDescription on Windows.
4. RT priority aplicado (skip si no admin).
5. Multiple threads concurrentes — independientes.
6. stop_and_join sin start es no-op (no crash).

### §17.7 Migración Ayama

- `ayama/core/include/ayama/core/InternalWatchdog.hpp::watcher_` (std::thread)
  → reemplazar con `DedicatedThread` con `opts.below_normal = true`.
- `ayama/tools/ayama-bench/workload.hpp` (N workers std::thread)
  → array de `DedicatedThread` con pin_core derivado de FR-3.

### §17.8 Estimación

- **Tamaño:** M (~150 LOC + 6 tests)
- **Dependencias:** FR-3 (`pin_current_thread`), `elevate_thread_rt`.
- **Riesgo:** bajo.

---

## §18 — FR-18: `gma::proc::CurrentProcess` — self-process helpers

### §18.1 Objetivo

Acceso portable y consistente a la información del propio proceso. Hoy se usa `GetCurrentProcessId()` en Windows y `getpid()` en POSIX en múltiples lugares.

### §18.2 API final

```cpp
// pillars/process/include/gamma/process/CurrentProcess.hpp
namespace gma::proc {

/// Current process ID (cached once at first call).
[[nodiscard]] uint32_t self_pid() noexcept;

/// Current parent process ID. 0 if unavailable.
[[nodiscard]] uint32_t self_ppid() noexcept;

/// Current process name (exe basename, null-terminated). Cached.
[[nodiscard]] const char* self_name() noexcept;

#ifdef _WIN32
/// Pseudo-handle (GetCurrentProcess() — doesn't need closing).
[[nodiscard]] void* self_handle_win32() noexcept;
#endif

} // namespace gma::proc
```

### §18.3 Pillar placement

`pillars/process` — mismo target que FR-4 (`ProcessEnumerator`).

### §18.4 Strategy

Function-local statics. Compute once, cache forever.

```cpp
uint32_t self_pid() noexcept {
    static const uint32_t pid = []() noexcept {
#ifdef _WIN32
        return static_cast<uint32_t>(GetCurrentProcessId());
#else
        return static_cast<uint32_t>(getpid());
#endif
    }();
    return pid;
}
```

### §18.5 Performance profile

Primera call: ~50 ns (1 syscall). Subsiguientes: ~2 ns (static load).

### §18.6 Tests requeridos

1. `self_pid() == ::getpid()` (POSIX) / `GetCurrentProcessId()` (Win).
2. Múltiples calls retornan el mismo valor (cached).
3. `self_name()` non-empty para el test executable.

### §18.7 Migración Ayama

`grep -r "GetCurrentProcessId\|getpid" ayama/` → ~6 sitios. Reemplazar.

### §18.8 Estimación

- **Tamaño:** XS (~40 LOC + 3 tests)
- **Dependencias:** ninguna.
- **Riesgo:** nulo.

---

## §19 — FR-19: `gma::tuning::set_self_working_set()` — memory budget enforcement

### §19.1 Objetivo

`AgentRuntime::apply_memory_budget()` hardcodea `SetProcessWorkingSetSize(GetCurrentProcess(), 16MB, 50MB)`. Centralizar en `tuning` pillar como herramienta reusable. Permite que otros agents Gamma apliquen presupuestos similares.

### §19.2 API final

```cpp
// pillars/tuning/include/gamma/tuning/WorkingSet.hpp
namespace gma::tuning {

/// Hint the OS about desired working set bounds for THIS process.
/// `min_bytes`/`max_bytes` are soft hints — kernel may exceed under pressure.
///
/// Windows: SetProcessWorkingSetSize. Requires SE_INC_BASE_PRIORITY_NAME on some SKUs.
/// Linux:   setrlimit(RLIMIT_RSS, ...) — soft limit only; ignored by modern kernels
///          (>4.x). Returns OK but logs a warning. No-op effectively.
///
/// Returns OK on success or accepted-noop. PermissionDenied if Win32 access denied.
[[nodiscard]] std::expected<void, gma::Error>
set_self_working_set(uint64_t min_bytes, uint64_t max_bytes) noexcept;

} // namespace gma::tuning
```

### §19.3 Pillar placement

`pillars/tuning` — junto a `PrivilegeCheck`.

### §19.4 Strategy

```cpp
#ifdef _WIN32
std::expected<void, Error> set_self_working_set(uint64_t min_b, uint64_t max_b) noexcept {
    if (SetProcessWorkingSetSize(GetCurrentProcess(),
            static_cast<SIZE_T>(min_b),
            static_cast<SIZE_T>(max_b)))
        return {};
    const DWORD e = GetLastError();
    return std::unexpected(Error{
        e == ERROR_ACCESS_DENIED ? ErrorCode::PermissionDenied : ErrorCode::SystemError});
}
#else
std::expected<void, Error> set_self_working_set(uint64_t, uint64_t max_b) noexcept {
    struct rlimit r{ static_cast<rlim_t>(max_b), static_cast<rlim_t>(max_b) };
    (void)setrlimit(RLIMIT_RSS, &r);
    return {};   // best-effort
}
#endif
```

### §19.5 Performance profile

Una syscall. ~5-10 µs. Llamado una vez en `start()`.

### §19.6 Tests requeridos

1. `set_self_working_set(16MB, 64MB)` succeed en proceso propio.
2. Valores `(0, 0)` → succeed (significa "release working set hint").
3. POSIX: no crash; retorna OK aunque sea no-op.

### §19.7 Migración Ayama

```cpp
// AgentRuntime::apply_memory_budget() ANTES:
SetProcessWorkingSetSize(GetCurrentProcess(),
    kMinMB * 1024u * 1024u, kMaxMB * 1024u * 1024u);

// DESPUÉS:
(void)gma::tuning::set_self_working_set(
    kMinMB * 1024ull * 1024ull, kMaxMB * 1024ull * 1024ull);
```

Cross-platform automático.

### §19.8 Estimación

- **Tamaño:** XS (~60 LOC + 3 tests)
- **Dependencias:** ninguna.
- **Riesgo:** nulo.

---

## §20 — Orden de implementación y dependencias

### §20.1 Grafo de dependencias

```
FR-2 (ccd_count)          ─┐
                           ├─► topology pillar — independent
FR-1 (topology singleton) ─┘

FR-3 (set_process_affinity) ─► topology pillar — independent

FR-4 (proc::enumerate)    ──► nuevo pillar `process` — independent

FR-5 (ShmRegion)          ──► nuevo pillar `ipc`     — independent

FR-6 (etw::SessionManager)──► nuevo pillar `etw`     — independent

FR-7 (node hot-restart)   ─► graph/runtime pillars — DEFERRED
```

Ninguno de los FRs depende de otro a nivel de código. Pueden implementarse en cualquier orden o en paralelo.

### §20.2 Orden recomendado por impacto vs esfuerzo

| Orden | FR | Esfuerzo | Impacto Ayama | Razón |
|---|---|---|---|---|
| 1 | FR-2 (`ccd_count`) | XS | M | Quick win, demuestra el patrón de cache-en-probe |
| 2 | FR-1 (`topology()`) | S | M | Quick win, mejora ergonomía cross-codebase |
| 3 | FR-3 (`set_process_affinity`) | M | H | Centraliza un código duplicado críticamente sensible (privileges) |
| 4 | FR-4 (`proc::enumerate`) | L | H | Hot path Ayama (cada 100 ms); migrar es ganancia inmediata |
| 5 | FR-5 (`ShmRegion`) | L | XH | Mayor ahorro de LOC en Ayama; permite reuso por futuros consumers |
| 6 | FR-6 (`etw::SessionManager`) | XL | H | Mayor refactor en MetricsCollector; ETW es Windows-only |
| 7 | FR-7 (hot-restart) | XXL | L | Deferred a v2 |

### §20.3 Milestones

**Milestone M1 — Quick wins (≤1 semana):** FR-2 + FR-1
**Milestone M2 — Process control (~1 semana):** FR-3
**Milestone M3 — Process enumeration (~2 semanas):** FR-4
**Milestone M4 — IPC abstraction (~3 semanas):** FR-5
**Milestone M5 — ETW abstraction (~4 semanas):** FR-6
**Future:** FR-7

Total estimado: ~10-12 semanas / 1 ingeniero, o ~6 semanas / 2 ingenieros en paralelo.

---

## §21 — Criterios de aceptación (checklist Gamma)

Toda PR que implemente cualquier FR debe satisfacer esta lista antes de merge. Es no-negociable.

### §21.1 API surface
- [ ] Todas las funciones públicas son `[[nodiscard]] noexcept`.
- [ ] Fallos usan `std::expected<T, gma::Error>`, nunca exceptions.
- [ ] Tipos POD pasan `static_assert(std::is_trivially_copyable_v<T>)`.
- [ ] `static_assert(sizeof(T) == ...)` para todos los PODs cross-IPC.
- [ ] Strings son `char[N]` en PODs, nunca `std::string`.

### §21.2 Performance
- [ ] Hot-path APIs no allocates (verificado con counter custom).
- [ ] Singletons usan `function-local static` (no `std::once_flag` manual).
- [ ] No hay `std::vector::push_back` en código llamado por loops.
- [ ] Cache-line alignment usado donde hay producer/consumer crossing.

### §21.3 Cross-platform
- [ ] Compila en Windows + Linux (incluso con stubs en pillares Windows-only).
- [ ] Tests pasan en ambos OS (CI matrix).
- [ ] `#ifdef _WIN32` solo en `.cpp`, no en headers públicos (excepto FR-6 ETW).

### §21.4 Pillar hygiene
- [ ] Nuevo pillar tiene `CMakeLists.txt` siguiendo el patrón de `pillars/topology/`.
- [ ] Headers en `include/gamma/<pillar>/`.
- [ ] Implementación en `src/`.
- [ ] Tests en `tests/`.
- [ ] Sin dependencias circulares (verificado con `cmake --graphviz`).

### §21.5 Documentation
- [ ] Cada header tiene `§Phase X.Y, gma::pillar` annotation.
- [ ] Header file leading-comment explica: qué hace, threading, resource, privilege.
- [ ] Migration notes para Ayama en el PR description.

### §21.6 Tests
- [ ] Cobertura: cada función pública testeada al menos una vez.
- [ ] Errores testeados: cada `ErrorCode` retornable tiene un test que lo dispara.
- [ ] Threading: TSan-clean para APIs llamadas desde múltiples threads.
- [ ] Performance: benchmark básico para hot-path APIs (microbenchmark).

---

## §22 — Riesgos cross-cutting

### §22.1 Windows API version compatibility

Gamma 1.0 targetea Windows 10 22H2+ / Windows 11. Esto cubre todas las APIs propuestas (`SetProcessAffinityMask` existe desde Vista, ETW desde XP, `EnumProcesses` desde 2000). **No hay riesgo significativo.**

### §22.2 Linux distro fragmentation

`/proc` parsing requiere kernel ≥3.10 (RHEL 7) — universalmente disponible. `sched_setaffinity` con PID externo requiere `CAP_SYS_NICE` o root. Tests CI corren como usuario normal → tests de cross-PID se skip silenciosamente.

### §22.3 ARM64 Windows

Gamma 1.0 tiene soporte preliminar ARM64 (mencionado en HardwareTopology.hpp). FR-3 (affinity) y FR-4 (process enum) funcionan idénticamente. FR-6 (ETW) también; solo la disponibilidad de providers difiere. **No es un blocker.**

### §22.4 Magic value collisions

FR-5 usa `magic` 32-bit. Espacio de 4B billones; colisión accidental es astronómicamente improbable. Recomendación: usar magic con caracteres ASCII reconocibles (`'AYAM'`, `'GAMM'`) — facilita debug con hexdump.

### §22.5 Concept compilation cost

FR-5 usa C++20 concepts. Esto incrementa tiempo de compilación marginalmente. Mitigación: el concept se evalúa una vez por instanciación de template, no por uso.

### §22.6 ETW privilege model changes

Microsoft ha tightened ETW security en updates recientes. Algunos providers requieren AppContainer-friendly tokens incluso con admin. Riesgo: provider X funciona en Win10 pero falla en Win11 24H2+. **Mitigación:** documentar provider compatibility en `providers::` namespace, hacer providers individuales fallable (no abortar toda la sesión por un provider).

---

## §23 — Verification matrix (CI)

### §23.1 Tests por FR

| FR | Unit tests | Integration test | Cross-platform | Performance test |
|---|---|---|---|---|
| FR-1 | 5 | reuse en cualquier consumer | Win + Linux | bench: 2nd call < 5ns |
| FR-2 | 4 | usado en FR-1 path | Win + Linux | N/A (trivial) |
| FR-3 | 5 | aplicar+restore en propio PID | Win + Linux | bench: < 10µs |
| FR-4 | 5 | enumerate auto-PID | Win + Linux | bench: 200procs < 5ms |
| FR-5 | 7 | producer+consumer fork test | Win + Linux | bench: read < 50ns |
| FR-6 | 6 | session+1 event capture | Win + Linux compile | bench: 1k events/sec |

### §23.2 CI matrix

```yaml
matrix:
  os: [windows-2022, ubuntu-22.04]
  build_type: [Release, Debug]
  sanitizer: [None, ASan+UBSan, TSan]   # TSan only on Linux Debug
```

### §23.3 Performance regression gates

Cada FR define un microbenchmark de aceptación. PR que regrese >10% versus baseline → fail. Baselines mantenidos en `pillars/<pillar>/benchmarks/baseline.csv`.

---

## §24 — Open questions a resolver antes de codear

Estas preguntas requieren respuesta del lead Gamma antes de comenzar implementación. Documentar resolución en este file o en un append-only `decisions.md`.

### §24.1 FR-1

**Q1.** ¿Debe `topology()` re-intentar el probe si la primera vez falló? (Caso: probe falla por race con OS startup, segundo intento podría succeed.)
**Recomendación:** No. Mantener simple. Si el caller quiere re-probe, expone `force_reprobe()` explícito.

### §24.2 FR-4

**Q2.** ¿`enumerate_processes()` debe filtrar processes del System (PID 4) y System Idle (PID 0) automáticamente?
**Recomendación:** No filter. Devolver todo lo que el OS reporta. El caller (Ayama) ya filtra por `kind == System` después.

**Q3.** Pillar name: `process` vs `proc` vs `procctl`?
**Recomendación:** `process` — alineado con `topology`, `transport`, `behavior` (todos sustantivos completos).

### §24.3 FR-5

**Q4.** ¿`ShmRegion` debe soportar `try_open()` (no esperar a que exista) vs `open_blocking()` (esperar)?
**Recomendación:** Solo `open()` non-blocking. Caller implementa reconnect loop. Es lo que ya hace `AyamaClient`.

**Q5.** ¿Soportar resize después de create?
**Recomendación:** No en v1. Layout estático. Si se necesita resize, recrear con magic bumped.

### §24.4 FR-6

**Q6.** ¿`SessionManager` debe ofrecer un built-in event ring buffer, o solo el callback?
**Recomendación:** Solo callback. Mantener mínimo. Cada consumer (Ayama) implementa su propio ring específico al uso.

**Q7.** ¿GUID database `providers::` debe estar en el header (compile-time constants) o en `.cpp`?
**Recomendación:** Header con `inline constexpr GUID`. Permite que el linker deduplique y los consumers los referencien sin link extra.

### §24.5 Cross-cutting

**Q8.** ¿Bump de Gamma version (1.0.0 → 1.1.0) tras estos FRs?
**Recomendación:** Sí, 1.1.0 — todos los FRs son additive (SemVer minor), pero juntos justifican un marker visible.

---

**Fin del Implementation Strategies document v0.1**

Última actualización: Mayo 2026
Próxima revisión: tras kickoff del Milestone M1.
