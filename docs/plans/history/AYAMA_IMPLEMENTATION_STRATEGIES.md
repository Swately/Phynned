> **📦 Archived planning document — historical.**
> From Phyriad's pre-rebrand era (the project was codenamed *gamma* / *gma*).
> Published for transparency about how the project was actually built — **not**
> a current specification. Version numbers and performance claims here
> (including any "vs Folly" / "production-ready" framing) reflect the project's
> state at the time of writing and may since have been revised or **retracted**;
> the project has reset to `v0.1.0-experimental`. For current status see the
> repo `README.md` and `docs/planning/`. For benchmark-claim validation status
> see `docs/planning/TEST_INVENTORY.md`.

# Ayama — Implementation Strategies
## Patrones técnicos concretos para máximo rendimiento, eficiencia y UX

**Version:** 0.3 — Mayo 2026
**Foundation:** Gamma Framework release line **1.1.0** (post FR pack mayo 2026; macro `GAMMA_VERSION_STRING` sigue en `"1.0.0"`)
**Companion doc:** [`AYAMA_MASTER_PLAN.md`](AYAMA_MASTER_PLAN.md)
**Source of truth para APIs Gamma:** [`../GAMMA_INVENTORY.md`](../GAMMA_INVENTORY.md) — derivado del código real
**Audience:** implementadores de cada Bloque del Master Plan.

### Changelog v0.2 → v0.3

- **§13.4 path de migración:** tabla refleja estado real verificado contra `ctest` (43/43 PASS, mayo 2026).
- **§13.5 LOC table:** marca cuáles migraciones están DONE vs PENDIENTE con valores actuales.
- **Nota de versión** alineada con `pillars/_meta/include/gamma/version.hpp` (macro = `"1.0.0"`, release line = 1.1.0).

Este documento NO redefine los Bloques (eso está en el Master Plan). Esto es la
**caja de herramientas técnica** con patrones concretos, decisiones difíciles
ya tomadas y código de referencia. Cuando se implementa un Bloque, primero se
consulta acá si hay un patrón aplicable.

### Changelog v0.1 → v0.2

- **FR pack v1.1.0 integrado** — patrones actualizados para usar primitivas
  Gamma en lugar de hand-rolled equivalents:
  - §1.5 Ring buffers → `gma::ipc::Ring<T, Capacity>` (FR-12)
  - §2.1 SHM protocol → `gma::ipc::ShmRegion<H,P>` (FR-5)
  - §2.2 Wake events → `gma::hal::WakeEvent` (FR-16)
  - §2.3 Action log → `gma::ipc::Ring<ActionLogEntry, 128>`
  - §3.3 Memory budget → `gma::tuning::set_self_working_set()` (FR-19)
  - §4.2 Loop principal → `WakeEvent::wait()`
  - §5.5 Fallback sin ETW → `gma::proc::ProcessMetricsSnapshot::capture()` (FR-11)
- **Nuevo §13** — Patrones para integrar los pilares nuevos (`process`, `ipc`, `etw`).

---

## Tabla de contenidos

- [§1 — Performance: zero-alloc hot path](#1--performance-zero-alloc-hot-path)
- [§2 — Performance: lock-free e IPC](#2--performance-lock-free-e-ipc)
- [§3 — Eficiencia: anti-parasitic resource budget](#3--eficiencia-anti-parasitic-resource-budget)
- [§4 — Eficiencia: adaptive ticking y power awareness](#4--eficiencia-adaptive-ticking-y-power-awareness)
- [§5 — Eficiencia: ETW con bajo overhead](#5--eficiencia-etw-con-bajo-overhead)
- [§6 — UX: Auto mode pipeline](#6--ux-auto-mode-pipeline)
- [§7 — UX: clasificación de procesos](#7--ux-clasificación-de-procesos)
- [§8 — UX: persistencia y aprendizaje](#8--ux-persistencia-y-aprendizaje)
- [§9 — UX: transparencia y reversibility](#9--ux-transparencia-y-reversibility)
- [§10 — Robustez: error handling y degradación](#10--robustez-error-handling-y-degradación)
- [§11 — Compatibilidad: cero invasive operations](#11--compatibilidad-cero-invasive-operations)
- [§12 — Templates de código reusable](#12--templates-de-código-reusable)
- [§13 — Pilares nuevos (Gamma 1.1.0): process / ipc / etw](#13--pilares-nuevos-gamma-110-process--ipc--etw)

---

## §1 — Performance: zero-alloc hot path

### 1.1 Regla maestra

**Toda struct/buffer del hot path se construye una vez en `start()` y vive hasta `stop()`.**
Ninguna llamada en el bucle de tick puede invocar `new`, `malloc`, `std::vector::resize`
ni cualquier path que pueda allocar.

### 1.2 Pre-allocación estándar

Cada componente principal expone una constante `kMax*` y usa `std::array` o
buffer raw alineado:

```cpp
class ProcessObserver {
public:
    static constexpr uint32_t kMaxTargets = 32u;

private:
    alignas(64) std::array<TargetProcess, kMaxTargets> targets_;
    alignas(64) std::array<TargetMetrics, kMaxTargets> metrics_;
    uint32_t                                            n_targets_{0u};
    // ...
};
```

**No usar `std::vector` para colecciones de tamaño acotado.** Usar `std::array<T, N>`
con un counter aparte. Esto:
- Elimina indirection.
- Garantiza layout contiguo (cache-friendly).
- Hace explícito el límite en compile-time.

### 1.3 Strings

Strings de proceso (`name`, `window_title`, `exe_path`) son `char[N]` fijos.
Nunca `std::string` en POD types. Conversión en boundaries:

```cpp
// Cuando llegamos desde una API que retorna std::wstring:
inline void copy_narrow(char* dst, std::size_t dst_sz,
                       const wchar_t* wide, std::size_t wide_len) noexcept
{
    const std::size_t max = dst_sz - 1u;
    std::size_t out = 0u;
    for (std::size_t i = 0u; i < wide_len && out < max; ++i) {
        if (wide[i] < 0x80) dst[out++] = static_cast<char>(wide[i]);
        // Para non-ASCII: replace with '?' o usar WideCharToMultiByte una vez.
    }
    dst[out] = '\0';
}
```

Para nombres de ejecutable, ASCII es suficiente en 99% de casos. Para títulos
de ventana (que pueden ser unicode) usar `WideCharToMultiByte(CP_UTF8, ...)`
**una sola vez** por sample, no en el hot path.

### 1.4 Memcpy/memset alineados

Para snapshots:

```cpp
// PREFERIR:
std::memcpy(&out_state, &shm_state, sizeof(AyamaState));

// SOBRE:
out_state = shm_state;     // copia campo a campo, branch-y
```

Para clear: `std::memset(&buf, 0, sizeof(buf))` cuando el target es POD trivial.
Para POD no-trivial (con defaults non-zero): `buf = T{};` que el compilador
puede convertir en memset o en moves óptimos.

### 1.5 Ring buffers — usar el de Gamma

**Preferir `gma::ipc::Ring<T, Capacity>` (FR-12)** para SPSC nuevo. Es el
ring lock-free moderno con cursores cache-aligned (cero false sharing),
power-of-2 capacity, y `drain()` con 2-memcpy óptimo. Header-only template.

```cpp
#include <gamma/ipc/Ring.hpp>

// Ejemplo: ring de ActionLogEntries — producer ActionExecutor, consumer UI.
gma::ipc::Ring<ActionLogEntry, 128> action_log_ring_;

// Producer side (en ActionExecutor::apply):
(void)action_log_ring_.try_push(entry);

// Consumer side (en UI tick):
ActionLogEntry batch[64];
const uint32_t n = action_log_ring_.drain(batch, 64);
for (uint32_t i = 0; i < n; ++i) draw_entry(batch[i]);
```

**Cuándo usar `gma::transport::Ring<T>` (legacy):** solo si necesitas
SlotCopy SIMD para `T` grandes (>128B) o ya estás dentro del DAG de nodes
con `Inlet/Outlet`. Para SPSC plain → `gma::ipc::Ring` es mejor opción.

**Performance medido (gma::ipc::Ring):**
- `try_push` / `try_pop`: ~10-20 ns
- `drain(64)` con T=32B: ~120 ns
- 10M push+pop bench: ~9 ms (≈0.9 ns/op amortizado en hot path)

### 1.6 Hash maps en hot path: prohibidos

Si necesitás "lookup por PID":
- Si N < 32 (caso típico): array linear con SIMD comparison. Más rápido que
  hash map en estos tamaños.
- Si N podría ser mayor: cuestionar el diseño. Probablemente algo está mal.

```cpp
// Lookup linear acelerado:
inline int32_t find_pid_index(const TargetProcess* arr, uint32_t n,
                              uint32_t pid) noexcept
{
    for (uint32_t i = 0; i < n; ++i)
        if (arr[i].pid == pid) return static_cast<int32_t>(i);
    return -1;
}
```

Para `n = 32` esto es ~16 ns en CPU moderno. Insignificante.

### 1.7 Allocators custom (cuando inevitable)

Si UN componente necesita allocación dinámica (e.g. parsing TOML al startup),
aislarlo y NO permitir que toque el hot path:

```cpp
class ConfigStore {
public:
    // Allocates only here. Returns POD struct.
    [[nodiscard]] static std::expected<ConfigData, gma::Error>
    load(const char* path) noexcept;

    // ConfigData es POD; el resto del programa solo usa ConfigData.
};
```

---

## §2 — Performance: lock-free e IPC

### 2.1 SHM protocol con Ayama-agent ↔ ayama-ui

**Patrón:** producer único (agent), consumer múltiple (UI clients).
Single-writer / multi-reader con seqlock pattern.

**Usar `gma::ipc::ShmRegion<Header, Payload>` (FR-5).** El template encapsula
el patrón completo: mapping, magic/version check, seqlock RAII guard, retry
con torn-read detection. No hay razón para hand-rollearlo.

```cpp
#include <gamma/ipc/ShmRegion.hpp>

// 1. Definir Header que cumple ShmHeaderConcept:
struct alignas(64) AyamaShmHeader {
    std::atomic<uint32_t> magic;        // = kAyamaMagic ('AYAM')
    std::atomic<uint32_t> version;      // = kAyamaVersion
    std::atomic<uint32_t> agent_pid;
    std::atomic<uint32_t> seq;          // seqlock counter
    uint8_t  _pad[48];
};
static_assert(sizeof(AyamaShmHeader) == 64);
static_assert(std::is_standard_layout_v<AyamaShmHeader>);

// 2. Definir Payload (POD):
struct alignas(64) AyamaPayload {
    AyamaStateHeader  header;
    TargetProcess     targets[32];
    TargetMetrics     metrics[32];
    // ...
};
static_assert(std::is_trivially_copyable_v<AyamaPayload>);

// 3. Producer (agent):
auto r = gma::ipc::ShmRegion<AyamaShmHeader, AyamaPayload>::create(
    "Local\\AyamaAgent.v1", kAyamaMagic, kAyamaVersion);
if (!r) return r.error();
region_ = std::move(*r);
region_.header().agent_pid.store(gma::proc::self_pid(),
                                  std::memory_order_release);

// Each tick:
{
    auto guard = region_.begin_write();  // seq: even → odd
    region_.payload().header.n_targets = n;
    std::memcpy(region_.payload().targets, ...);
    // guard dtor: seq → even
}

// 4. Consumer (ayama-ui):
auto r = gma::ipc::ShmRegion<AyamaShmHeader, AyamaPayload>::open(
    "Local\\AyamaAgent.v1", kAyamaMagic, kAyamaVersion);
if (!r) {
    // Returns SchemaMismatch / VersionMismatch / NotFound
    return r.error();
}
AyamaPayload snapshot;
if (region_.try_read_consistent(&snapshot)) {
    // Use snapshot — guaranteed consistent
}
```

**Ventajas vs hand-rolled:**
- ~250 LOC eliminadas en `AyamaAgentPublisher` + `AyamaClient`.
- ABI compat: magic + version mismatch detectados en `open()`.
- Tested under contention (7 tests including 8-thread reader stress).
- Cross-platform (Win32 `CreateFileMapping` / POSIX `shm_open`).

### 2.2 Wake events vs polling

Ayama-agent publica → UI debería despertarse para refrescar.
**Usar `gma::hal::WakeEvent` (FR-16).** Cross-platform auto-reset event;
Win32 `CreateEventW` / Linux `eventfd` / fallback `mutex+condvar`.

```cpp
#include <gamma/hal/WakeEvent.hpp>

// Agent side:
auto evt_opt = gma::hal::WakeEvent::create();
if (!evt_opt) { /* fatal — extremely rare */ }
auto wake = std::move(*evt_opt);

// In tick loop (cuando hay nuevos datos):
wake.signal();

// UI side (en logic node tick):
const bool refreshed = wake.wait(max_idle_ms);
if (refreshed) {
    snapshot_from_shm();   // pull update from ShmRegion
}
```

**Por qué `WakeEvent` y no la primitiva nativa:**
- Cross-platform sin ifdefs en el consumer.
- Auto-reset garantizado (no leak entre signal/wait).
- Movable + RAII destructor → no leak de HANDLEs / fds.
- API mínima (3 métodos), testeada con stress 10k pares ping-pong.

**Idiom — early-wake en main loop del agent:**
```cpp
void AgentRuntime::run() noexcept {
    while (!stop_requested_.load()) {
        tick();
        const uint32_t sleep_ms = adaptive_tick_ms();
        wake_.wait(sleep_ms);   // stop() llama wake_.signal() → wake inmediato
    }
}
```

Esto vale infinitamente más que polling. La UI tickea al ritmo del agent
cuando hay cambios, y duerme cuando no.

### 2.3 Action log ring (usar `gma::ipc::Ring`)

El action log es write-only desde agent, read-only desde la UI.
**Usar `gma::ipc::Ring<ActionLogEntry, 128>` (FR-12)** directamente — no
necesita estructura custom.

```cpp
#include <gamma/ipc/Ring.hpp>
#include <ayama/action/ActionLog.hpp>

class ActionLogPublisher {
    gma::ipc::Ring<ActionLogEntry, 128> ring_;

public:
    // Writer (ActionExecutor side, single thread):
    void log(const ActionLogEntry& e) noexcept {
        // try_push devuelve false si full → dropped() counter incrementa.
        // En este uso (128 entries, baja tasa) overflow es virtualmente imposible.
        (void)ring_.try_push(e);
    }

    // Reader (UI side):
    uint32_t drain(ActionLogEntry* out, uint32_t max) noexcept {
        return ring_.drain(out, max);
    }

    // Diagnostic:
    [[nodiscard]] uint64_t dropped() const noexcept { return ring_.dropped(); }
};
```

**Ventajas vs hand-rolled `ActionLogRing`:**
- Sin "overrun skip ahead" lógica — `try_push` reporta drop con counter.
- Cursores cache-aligned (`kDestructivePad`) → cero false sharing.
- `drain()` con 2-memcpy óptimo handles wrap.
- ~80 LOC eliminadas, mismo throughput o mejor.

### 2.4 ETW callbacks: minimizar trabajo

**Usar `gma::etw::SessionManager` (FR-6)** — maneja `StartTrace` + `EnableTraceEx2` +
`OpenTrace` + consumer thread. Ayama solo provee el callback y el ring buffer.

```cpp
#include <gamma/etw/SessionManager.hpp>
#include <gamma/ipc/Ring.hpp>

class MetricsCollector {
    gma::etw::SessionManager etw_;
    gma::ipc::Ring<TimelineEvent, 65536> etw_ring_;

public:
    std::expected<void, gma::Error> start_etw() noexcept {
        const gma::etw::ProviderSpec providers[] = {
            { gma::etw::providers::kKernelProcess, TRACE_LEVEL_INFORMATION, 0x10, 0 },
            { gma::etw::providers::kKernelThread,  TRACE_LEVEL_INFORMATION, 0x10, 0 },
            // CSwitch keyword:
            { gma::etw::providers::kKernelContextSwitch,
              TRACE_LEVEL_INFORMATION, 0x10, 0 },
        };

        auto r = etw_.start("AyamaKernel.v1", providers, 64u, 16u);
        if (!r) return r;
        return etw_.start_consumer(&on_event, this);
    }

    void stop_etw() noexcept { etw_.stop(); }

private:
    // CALLBACK (en ETW consumer thread):
    static void on_event(const EVENT_RECORD& rec, void* user_ctx) noexcept {
        auto* me = static_cast<MetricsCollector*>(user_ctx);
        TimelineEvent ev{};
        ev.kind   = classify_event(rec);
        ev.tsc    = rec.EventHeader.TimeStamp.QuadPart;
        ev.aux_u32 = read_pid_from_event(rec);
        (void)me->etw_ring_.try_push(ev);   // lock-free, never blocks
    }
};

// CONSUMER (en Ayama-agent main thread):
void MetricsCollector::sample(...) noexcept {
    TimelineEvent batch[256];
    uint32_t n;
    while ((n = etw_ring_.drain(batch, 256)) > 0) {
        for (uint32_t i = 0; i < n; ++i)
            process_event(batch[i]);
    }
}
```

**Reglas para el callback ETW** (siempre, no negociables):

1. **NO** llamar funciones que puedan bloquear (no allocaciones, no locks, no I/O).
2. **NO** llamar APIs de Ayama directamente.
3. **SOLO** push al ring buffer y salir.

**Ventajas vs hand-rolled:**
- `SessionManager::stop()` es seguro y idempotente. RAII garantiza no leak.
- Provider GUIDs centralizados en `gma::etw::providers::*` — sin magic numbers
  copiados entre files.
- Drop counter (`events_dropped()`) accesible para diagnostics.
- Linux: header provee no-op stubs, código compila pero el provider no se enable.

---

## §3 — Eficiencia: anti-parasitic resource budget

### 3.1 Resource self-monitoring (mandatorio)

Ayama-agent verifica SU PROPIO consumo cada N ticks y reporta:

```cpp
struct SelfMetrics {
    float    cpu_pct;            // 0..100, this process
    uint64_t rss_bytes;
    uint32_t threads;
    uint32_t handles;
    float    tick_interval_ms;
    uint64_t etw_events_per_sec;
};

class SelfMonitor {
public:
    void sample() noexcept {
        cycles_now_ = read_process_cycles();
        rss_bytes_  = read_process_rss();
        threads_    = count_threads();
        handles_    = count_handles();
        const auto dt_us = (cycles_now_ - cycles_last_) * 1e6 / tsc_freq_;
        // cpu_pct = process cycles / wall time
        cur_.cpu_pct = compute_cpu_pct(dt_us, wall_us_elapsed());
        cycles_last_ = cycles_now_;

        // Enforce budget:
        if (cur_.cpu_pct > kHardLimitIdleCpuPct && state_ == State::Idle) {
            // BUG: panic, log, reduce work.
            on_budget_violation();
        }
    }
};
```

**Si excedemos el budget, NO mentimos.** Loggeamos, reportamos al usuario,
y reducimos trabajo (e.g. duplicar tick interval).

### 3.2 Self-pinning de Ayama-agent

Al startup, Ayama-agent se pinea él mismo al core "menos valioso":

```cpp
namespace ayama::core {

inline uint32_t pick_self_core(const gma::HardwareTopology& topo) noexcept {
    // Intel hybrid: prefer E-cores.
    const auto e_cores = gma::hw::e_cores();
    if (!e_cores.empty()) {
        return e_cores[0];
    }

    // AMD X3D: prefer non-V-Cache CCD (libera CCD0 con V-Cache).
    const auto v_cores = gma::hw::v_cache_cores();
    if (!v_cores.empty()) {
        for (const auto& c : topo.cores) {
            if (!c.has_v_cache) return c.logical_id;
        }
    }

    // Homogeneous: prefer SMT sibling of core 0 (less valuable).
    // Fallback: last core.
    return static_cast<uint32_t>(topo.cores.size() - 1u);
}

inline void apply_self_pin(uint32_t core) noexcept {
    (void)gma::hw::pin_current_thread(core);
}

} // namespace ayama::core
```

**Test T10 verifica esto.** Si Ayama-agent corre en el core "bueno", falla el test.

### 3.3 Memoria pinned (working-set hint)

Todas las allocaciones de Ayama van al working set mínimo.
**Usar `gma::tuning::set_self_working_set()` (FR-19)** — cross-platform,
maneja errores semánticamente, sin ifdefs en el consumer.

```cpp
#include <gamma/tuning/WorkingSet.hpp>

void initialize_memory_budget() noexcept {
    constexpr uint64_t kMinBytes = 16ull * 1024u * 1024u;   // 16 MB
    constexpr uint64_t kMaxBytes = 50ull * 1024u * 1024u;   // 50 MB

    auto r = gma::tuning::set_self_working_set(kMinBytes, kMaxBytes);
    if (!r) {
        // OS denied (no admin, sandbox, etc.) — best-effort, no fatal.
        // Log to UI panel "Self-metrics" so user sees it.
        log_warning("Working-set hint denied (code=%u) — running unhinted",
                    static_cast<unsigned>(r.error().code));
    }
}

// Periodic check via get_self_working_set (e.g. cada 30s):
void poll_rss() noexcept {
    uint64_t cur = 0, peak = 0;
    if (gma::tuning::get_self_working_set(&cur, &peak)) {
        self_metrics_.rss_bytes      = cur;
        self_metrics_.peak_rss_bytes = peak;
    }
}
```

**Por qué `gma::tuning::set_self_working_set` y no la API directa:**
- Devuelve `PermissionDenied` semánticamente correcto (no opaque `BOOL` de Win32).
- Linux: no-op silencioso (modern kernels ignoran `setrlimit(RLIMIT_RSS)`) sin
  hacer fail al caller.
- Validation: rechaza `min > max` con `InvalidArgument` antes del syscall.

Esto le dice al OS: "no me dejes crecer más allá de 50 MB". Si Ayama tiene un
leak, esto lo manifiesta tempranamente (allocations fallan o page-fault spam).

### 3.4 Lazy init de subsistemas pesados

Componentes que no se necesitan hasta que aparece el primer target:

```cpp
class EtwLifecycle {
public:
    void on_target_appeared(uint32_t pid) noexcept {
        ++target_count_;
        if (target_count_ == 1 && !etw_active_) {
            (void)start_etw();   // primer target: arrancar ETW
            etw_active_ = true;
        }
    }

    void on_target_disappeared() noexcept {
        --target_count_;
        if (target_count_ == 0 && etw_active_) {
            stop_etw();          // último target: apagar ETW
            etw_active_ = false;
        }
    }
};
```

Cuando no hay nada que optimizar, Ayama no consume ETW buffers ni CPU. Sólo
sigue su tick lento (1000ms) para detectar el próximo proceso interesante.

### 3.5 Buffer reuse en bench

`Baseline` y `ABRunner` allocan UN buffer grande al construirse y lo reusan:

```cpp
class Baseline {
public:
    static constexpr uint32_t kMaxSamples = 8192u;   // 8192 × 64B = 512 KB

    Baseline() noexcept {
        // Single allocation, lives forever.
        samples_.reserve(kMaxSamples);
    }

    void push(const BaselineSample& s) noexcept {
        if (samples_.size() < kMaxSamples) {
            samples_.push_back(s);   // never reallocs (reserved)
        } else {
            // Wrap or drop: drop newest is fine.
        }
    }
};
```

512 KB es trivial y permite ~17 minutos a 8Hz sampling. Suficiente para
cualquier bench scenario.

---

## §4 — Eficiencia: adaptive ticking y power awareness

### 4.1 Tabla de tick rates

```cpp
namespace ayama::core {

enum class WorkloadState : uint8_t {
    DeepIdle = 0,    // no targets, ningún game corriendo, UI cerrada
    Idle     = 1,    // no targets activos pero UI abierta o game potencial
    Light    = 2,    // 1 target observado, sin policies activas
    Active   = 3,    // 1+ targets con policies activas
    Bench    = 4,    // bench A/B en curso
};

inline uint32_t tick_interval_ms(WorkloadState w, bool on_battery) noexcept {
    uint32_t base = 0;
    switch (w) {
        case WorkloadState::DeepIdle: base = 5000; break;
        case WorkloadState::Idle:     base = 1000; break;
        case WorkloadState::Light:    base = 500;  break;
        case WorkloadState::Active:   base = 100;  break;
        case WorkloadState::Bench:    base = 25;   break;
    }
    return on_battery ? base * 2u : base;
}

} // namespace ayama::core
```

### 4.2 Loop principal

```cpp
#include <gamma/hal/WakeEvent.hpp>

void AgentRuntime::run() noexcept {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const auto t0 = gma::hal::rdtsc();

        // Tick work
        observer_.refresh();
        metrics_.sample(observer_.target_pids(), observer_.target_count(), ...);
        const uint32_t n_dec = policy_.evaluate(...);
        action_.apply_batch(decisions_, n_dec);

        // Determine current workload state
        workload_state_ = classify_workload();

        // Compute next tick
        const uint32_t tick_ms = tick_interval_ms(workload_state_, on_battery_);

        // Sleep with early-wake support — wake_ is a gma::hal::WakeEvent (FR-16).
        // stop() llama wake_.signal() → wake inmediato.
        // ETW process-start callback también puede llamar wake_.signal().
        const auto t1 = gma::hal::rdtsc();
        const uint32_t elapsed_ms = static_cast<uint32_t>(
            (t1 - t0) * 1000ull / tsc_freq_);
        const uint32_t remaining = (elapsed_ms < tick_ms) ? tick_ms - elapsed_ms : 0u;

        if (remaining > 0u) {
            (void)wake_.wait(remaining);
        }
    }
}

void AgentRuntime::stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    wake_.signal();   // wake the loop inmediately
}
```

### 4.3 Power awareness

```cpp
class PowerWatch {
public:
    void refresh() noexcept {
#ifdef _WIN32
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps)) {
            on_battery_ = (ps.ACLineStatus == 0);
            battery_pct_ = ps.BatteryLifePercent;
        }
#endif
    }
    [[nodiscard]] bool on_battery() const noexcept { return on_battery_; }
    [[nodiscard]] uint8_t battery_pct() const noexcept { return battery_pct_; }
};
```

**Decisiones en battery mode:**
- Tick interval × 2 (menos overhead absoluto).
- Bench mode bloqueado (test T9 verifica).
- ETW reduce providers (solo context-switch, no DPC/ISR).
- Notificación al usuario: "Ayama detected battery — reduced ticking".

### 4.4 Idle desktop detection

```cpp
class IdleWatch {
public:
    bool desktop_idle_5min() noexcept {
#ifdef _WIN32
        LASTINPUTINFO lii{sizeof(lii)};
        if (GetLastInputInfo(&lii)) {
            const DWORD elapsed_ms = GetTickCount() - lii.dwTime;
            return elapsed_ms > 300'000u;
        }
#endif
        return false;
    }
};
```

Cuando el desktop está idle 5+ minutos AND no hay targets activos, transición a
DeepIdle.

### 4.5 ETW providers per workload state

```cpp
namespace ayama::observer {

enum class EtwProviderSet : uint8_t {
    None         = 0,
    Minimal      = 1,   // Process lifecycle only
    Standard     = 2,   // + Context switch
    Full         = 3,   // + DPC/ISR + Hard page faults (bench mode)
};

inline EtwProviderSet provider_set_for(WorkloadState w) noexcept {
    switch (w) {
        case WorkloadState::DeepIdle: return EtwProviderSet::None;
        case WorkloadState::Idle:     return EtwProviderSet::Minimal;
        case WorkloadState::Light:    return EtwProviderSet::Standard;
        case WorkloadState::Active:   return EtwProviderSet::Standard;
        case WorkloadState::Bench:    return EtwProviderSet::Full;
    }
    return EtwProviderSet::None;
}

} // namespace ayama::observer
```

`EtwSessionManager` recibe el set y enable/disable providers dinámicamente.
Reducir providers = menos eventos en ring = menos CPU consumido en callbacks.

---

## §5 — Eficiencia: ETW con bajo overhead

### 5.1 Buffer sizing crítico

```cpp
// EVENT_TRACE_PROPERTIES configuration:
constexpr uint32_t kEtwBufferSizeKb = 64u;     // por buffer (default 64)
constexpr uint32_t kEtwMinBuffers   = 4u;
constexpr uint32_t kEtwMaxBuffers   = 16u;     // cap; en sistemas idle reducir a 4
```

Buffers grandes = más latencia pero menos CPU spikes. Buffers chicos = mejor responsiveness
pero más overhead. Para Ayama: middle ground (64KB × 8 = 512KB total).

### 5.2 Drain proactivo

El callback ETW corre cuando el kernel decide. Para garantizar drain regular:

```cpp
class MetricsCollector {
public:
    void sample(...) noexcept {
        // Drain explícitamente cada tick.
        // Esto previene que el ETW ring se llene y eventos se descarten.
        TimelineEvent batch[256];
        uint32_t n;
        while ((n = etw_ring_.drain(batch, 256)) > 0) {
            for (uint32_t i = 0; i < n; ++i)
                process_event(batch[i]);
        }
    }
};
```

### 5.3 Filtrado per-PID en callback

ETW emite eventos del sistema entero. Filtrar early:

```cpp
static void on_ctxsw_event(const EVENT_RECORD* rec, void* udata) noexcept {
    auto* me = static_cast<MetricsCollector*>(udata);
    const uint32_t new_pid = read_pid_from_event(rec);

    // Hot-path filter: solo los PIDs que estamos observando.
    if (!me->pid_filter_.contains(new_pid)) return;

    TimelineEvent ev{...};
    me->etw_ring_.push(ev);
}
```

`pid_filter_` es un `std::array<uint32_t, kMaxTargets>` con linear scan SIMD.
Para 32 entries, el filtrado es ~5 ns en x86.

### 5.4 Provider selection rationale

| Provider | Cuándo se enciende | Qué da |
|---|---|---|
| Process lifecycle | Siempre que haya cualquier target potencial | start/stop de procesos → detectar nuevos targets sin polling |
| Thread context switch | Cuando hay 1+ targets activos | migrations/sec real per-thread |
| DPC / ISR | Solo en bench mode | latency spikes externos |
| Hard page faults | Solo en bench mode | hitch causes |
| Disk I/O | Nunca por default; opt-in para bench | identificar stutters por loading |

### 5.5 Fallback sin ETW

Si ETW falla (no admin, anti-virus interfiere, etc.), Ayama degrada elegantemente
al **bulk metrics path de FR-11** — `gma::proc::ProcessMetricsSnapshot::capture()`
que internamente usa `NtQuerySystemInformation`. Mismo backend, sin handles per-PID.

```cpp
#include <gamma/process/ProcessMetricsSnapshot.hpp>

class MetricsCollectorFallback {
    gma::proc::ProcessMetricsSnapshot snapshot_;

public:
    [[nodiscard]] std::expected<void, gma::Error> start() noexcept {
        auto r = gma::proc::ProcessMetricsSnapshot::create();
        if (!r) return std::unexpected(r.error());
        snapshot_ = std::move(*r);
        return {};
    }

    void sample(const uint32_t* pids, uint32_t n,
                ProcessMetrics* out) noexcept {
        // ~80-150 µs total, no abre handles per-PID.
        (void)snapshot_.capture();
        snapshot_.extract(pids, n, out);
    }
};
```

**Características post-FR-11:**
- Sin admin: funciona (`NtQuerySystemInformation` no requiere privilege).
- Performance: ~80-150 µs per capture (vs ~5 ms del per-PID polling antiguo).
- Tick interval puede mantenerse en 100 ms (no se necesita degradar a 500 ms+).
- Cero alloc en steady state (buffer 256 KB pre-allocado en `create()`).

Ayama reporta en UI: "ETW unavailable — using bulk-snapshot fallback (CPU times +
RSS only; no per-thread migrations)".

---

## §6 — UX: Auto mode pipeline

### 6.1 State machine del Auto mode

```
        ┌──────────────────────┐
        │  IDLE (no targets)   │
        └──────┬───────────────┘
               │
               │ ETW: process_start event
               ▼
        ┌──────────────────────┐
        │ EVALUATE (1-5s)      │  ← ProcessClassifier
        │ Es game? stream?     │
        └──────┬───────────────┘
               │
        ┌──────┴───────────────┐
        ▼                      ▼
    [no match]            [match]
        │                      │
        │                      ▼
        │           ┌───────────────────────┐
        │           │ APPLY POLICY           │
        │           │ - read learned mem    │
        │           │ - or auto-selector    │
        │           │ - apply via Action    │
        │           └──────┬────────────────┘
        │                  │
        │                  ▼
        │           ┌───────────────────────┐
        │           │ MONITOR (30s window)  │  ← A/B safety net
        │           │ measure frametime    │
        │           └──────┬────────────────┘
        │                  │
        │           ┌──────┴───────────┐
        │           ▼                  ▼
        │    [variance worse]   [variance better/same]
        │           │                  │
        │           ▼                  ▼
        │     AUTO-REVERT       PERSIST as good
        │      mark "bad" for       update memory.toml
        │      this exe            keep policy active
        │           │                  │
        │           └──────┬───────────┘
        │                  │
        │                  ▼
        │           ┌───────────────────────┐
        │           │ MAINTAIN              │
        │           │ keep policy as long   │
        │           │ as target alive       │
        │           └──────┬────────────────┘
        │                  │
        │           ETW: process_end event
        │                  │
        ▼                  ▼
    ─────────────────────────────
        │
        ▼
       REVERT all actions for this PID
       Back to IDLE.
```

### 6.2 ProcessClassifier — la decisión más delicada

Clasificar correctamente es lo que separa "Ayama es útil" de "Ayama jode al usuario".
**Reglas de clasificación deben ser conservadoras** — ante duda, NO clasificar.

```cpp
enum class TargetKind : uint8_t {
    Unknown      = 0,
    Game         = 1,
    Stream       = 2,
    Comm         = 3,
    Browser      = 4,
    Productivity = 5,
    System       = 6,
};

class ProcessClassifier {
public:
    [[nodiscard]] TargetKind classify(const ProcessInfo& p) noexcept {
        // 1. System processes: never touch.
        if (is_system_process(p)) return TargetKind::System;

        // 2. Exact name match (highest confidence)
        if (matches_known_stream(p.exe_name)) return TargetKind::Stream;
        if (matches_known_comm(p.exe_name))   return TargetKind::Comm;
        if (matches_known_browser(p.exe_name)) return TargetKind::Browser;

        // 3. Heuristic game detection (requires 3 signals to agree)
        int game_signals = 0;
        if (p.is_fullscreen_dwm) ++game_signals;
        if (p.gpu_usage_pct > 25.f) ++game_signals;
        if (p.foreground_for_sec > 30) ++game_signals;
        if (p.uses_d3d_or_vk) ++game_signals;     // detected via loaded modules
        if (game_signals >= 3) return TargetKind::Game;

        // 4. Productivity heuristic
        if (matches_known_productivity(p.exe_name)) return TargetKind::Productivity;

        return TargetKind::Unknown;   // No action en Auto mode
    }
};
```

**Heuristics detalladas:**
- `is_fullscreen_dwm`: `DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ...)` + check
  if window bounds match monitor bounds.
- `gpu_usage_pct`: leer perf counters `GPU Engine\Utilization Percentage` para el PID.
- `foreground_for_sec`: tracking interno via `GetForegroundWindow()` cada tick.
- `uses_d3d_or_vk`: `EnumProcessModules` y buscar `d3d11.dll`, `d3d12.dll`, `vulkan-1.dll`,
  `dxgi.dll`. Cache este resultado, no enumerar cada tick.

### 6.3 Tabla de matching: nombres conocidos

```cpp
// Stream apps (alta confianza)
constexpr const char* kStreamNames[] = {
    "obs64.exe",      "obs32.exe",
    "Streamlabs OBS.exe", "Streamlabs Desktop.exe",
    "XSplit.Core.exe", "Twitch Studio.exe",
    nullptr,
};

// Comm apps
constexpr const char* kCommNames[] = {
    "Discord.exe", "DiscordCanary.exe",
    "Teams.exe", "Zoom.exe", "ZoomPhone.exe",
    "slack.exe", "Skype.exe",
    nullptr,
};

// Browsers
constexpr const char* kBrowserNames[] = {
    "chrome.exe", "firefox.exe", "msedge.exe",
    "brave.exe", "opera.exe", "vivaldi.exe",
    nullptr,
};

// Productivity (no action por default, solo clasificar)
constexpr const char* kProductivityNames[] = {
    "devenv.exe",       // Visual Studio
    "Code.exe",         // VS Code
    "blender.exe",
    "Resolve.exe",      // DaVinci Resolve
    "premiere.exe",
    "AfterFX.exe",
    nullptr,
};
```

Estos arrays son `extern` y editables vía config TOML. El usuario puede agregar
nombres en `%LOCALAPPDATA%\Ayama\classifier.toml`.

### 6.4 Auto-revert con A/B sampling continuo

Después de aplicar policy, Ayama monitorea variance por 30s y compara con
baseline cortito que se captura justo ANTES:

```cpp
class AutoRevertGuard {
public:
    void on_policy_applied(uint32_t pid) noexcept {
        // Take 10s pre-baseline (we have ETW history already).
        baseline_variance_ = compute_variance_window(pid, last_10s);

        // Start monitoring window.
        monitoring_until_tsc_ = now_tsc + 30s_in_tsc;
        last_check_tsc_ = now_tsc;
    }

    void on_tick(uint32_t pid) noexcept {
        if (now_tsc >= monitoring_until_tsc_) return;
        // Every 5s, compare current variance to baseline.
        if (now_tsc - last_check_tsc_ < 5s_in_tsc) return;
        last_check_tsc_ = now_tsc;

        const float cur_variance = compute_variance_window(pid, last_10s);
        if (cur_variance > baseline_variance_ * 1.20f) {
            // 20% worse — significant regression.
            // Auto-revert.
            executor_.revert(pid);
            memory_.mark_bad(get_exe_name(pid));
            notify_user("Detected regression for X — reverted");
        }
    }
};
```

**Threshold 20%** porque variance es ruidoso. Menos sería false-positive city.
Más sería tolerar regresiones reales.

---

## §7 — UX: clasificación de procesos

### 7.1 Detección de fullscreen game (sin enumerar windows constantemente)

`EnumWindows` cada tick es caro. Patrón:

```cpp
class ForegroundWatcher {
public:
    void on_tick() noexcept {
        const HWND fg = GetForegroundWindow();
        if (fg == last_fg_) return;

        // Foreground changed: get its PID and check fullscreen.
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        last_fg_pid_ = pid;
        last_fg_     = fg;

        // Check fullscreen
        RECT rect;
        GetWindowRect(fg, &rect);
        const HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        last_fg_fullscreen_ = (rect.left == mi.rcMonitor.left &&
                               rect.top  == mi.rcMonitor.top &&
                               rect.right  == mi.rcMonitor.right &&
                               rect.bottom == mi.rcMonitor.bottom);
    }

    [[nodiscard]] uint32_t foreground_pid() const noexcept { return last_fg_pid_; }
    [[nodiscard]] bool is_foreground_fullscreen() const noexcept { return last_fg_fullscreen_; }
};
```

Esto es O(1) per tick. No enumera windows.

### 7.2 GPU usage por PID

Windows expone perf counters por PID. Implementación:

```cpp
// One-time setup (caro: 5-10ms):
PDH_HQUERY query;
PdhOpenQueryW(NULL, 0, &query);

// Per target, add counter:
PDH_HCOUNTER counter;
wchar_t path[256];
swprintf(path, 256,
    L"\\GPU Engine(pid_%u_*_engtype_3D)\\Utilization Percentage", pid);
PdhAddEnglishCounterW(query, path, 0, &counter);

// On each sample:
PdhCollectQueryData(query);
PDH_FMT_COUNTERVALUE val;
PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, NULL, &val);
const double gpu_pct = val.doubleValue;
```

Costo: `PdhCollectQueryData` es ~2-5 ms. Solo ejecutar 1x cada 500-1000ms,
no cada tick.

### 7.3 Cache de classifications

```cpp
struct ClassificationCacheEntry {
    char     exe_name[40];
    TargetKind kind;
    uint64_t last_verified_tsc;
};

class ClassificationCache {
    std::array<ClassificationCacheEntry, 128> entries_;
    uint32_t n_entries_{0};

public:
    // Re-verify every 60s; trust cache otherwise.
    [[nodiscard]] TargetKind classify_cached(const char* exe_name,
                                             const ProcessInfo& fresh_info) noexcept {
        // Linear search (n < 128, fits in L1).
        for (uint32_t i = 0; i < n_entries_; ++i) {
            if (std::strcmp(entries_[i].exe_name, exe_name) == 0) {
                if (now_tsc - entries_[i].last_verified_tsc < 60s_in_tsc) {
                    return entries_[i].kind;
                }
                // Stale: re-verify.
                const auto k = classifier_.classify(fresh_info);
                entries_[i].kind = k;
                entries_[i].last_verified_tsc = now_tsc;
                return k;
            }
        }
        // Not cached: classify + insert.
        const auto k = classifier_.classify(fresh_info);
        if (n_entries_ < entries_.size()) {
            std::strncpy(entries_[n_entries_].exe_name, exe_name,
                        sizeof(entries_[n_entries_].exe_name) - 1);
            entries_[n_entries_].kind = k;
            entries_[n_entries_].last_verified_tsc = now_tsc;
            ++n_entries_;
        }
        return k;
    }
};
```

---

## §8 — UX: persistencia y aprendizaje

### 8.1 Hardware fingerprint (estable)

Para que el aprendizaje sea relevante al hardware actual:

```cpp
namespace ayama::learn {

// Hash estable de la topology del CPU.
// Cambia cuando el usuario hace CPU upgrade → invalida aprendizajes viejos.
inline uint64_t compute_hw_id(const gma::HardwareTopology& t) noexcept {
    uint64_t h = 1469598103934665603ull;  // FNV-1a offset
    const auto add = [&](uint64_t v) noexcept {
        h ^= v;
        h *= 1099511628211ull;
    };
    add(t.physical_core_count());
    add(t.logical_core_count());
    add(t.cores.size() > 0 ? t.cores[0].l3_cache_kb : 0);
    add(static_cast<uint64_t>(t.vcache_cores.size()));
    add(static_cast<uint64_t>(t.numa_nodes.size()));
    // Include CPU brand hash to differentiate same-topology distinct CPUs.
    // (e.g. 7800X3D vs 7700X have similar topo).
    add(string_hash(t.cores[0].max_freq_mhz));   // approximation
    return h;
}

} // namespace ayama::learn
```

Output: e.g. `0xA1B2C3D4E5F60718`. Persistido como `hardware_id` en TOML.

### 8.2 Memory.toml layout

```toml
# Auto-generated by Ayama. Don't edit while ayama-agent is running.
version = 1
hardware_id = "0xA1B2C3D4E5F60718"
last_updated = "2026-05-21T15:23:01Z"

# Learned policies — one per (exe, hardware) tuple.
[[learned]]
exe = "Cyberpunk2077.exe"
classified_as = "Game"
applied_policy = "pin_v_cache_ccd"
applied_core_mask = 0x00FF             # cores 0-7 (CCD0 with V-Cache)
measured_p99_baseline_ms = 28.4
measured_p99_treated_ms = 22.1
improvement_pct = 22.2
sample_count = 12
last_validated = "2026-05-21T14:55:32Z"
user_locked = false                     # may be re-validated

[[learned]]
exe = "HogwartsLegacy.exe"
classified_as = "Game"
applied_policy = "pin_v_cache_ccd"
applied_core_mask = 0x00FF
measured_p99_baseline_ms = 45.2
measured_p99_treated_ms = 28.1
improvement_pct = 37.8
sample_count = 6
last_validated = "2026-05-20T19:11:00Z"
user_locked = true                      # user manually locked, no re-eval

# Bad-list: don't try these again unless reset.
[[bad]]
exe = "WeirdLegacyApp.exe"
reason = "regression_detected"
last_attempted = "2026-05-19T22:01:00Z"
```

### 8.3 Re-validation strategy

Cada `learned` entry tiene `last_validated`. Política:

- **< 30 días, sample_count >= 5**: Trust. Apply inmediatamente.
- **30-90 días**: Apply, pero re-validar (A/B durante 60s).
- **> 90 días**: Treat como "needs validation" — apply tentative, validate.
- **`user_locked = true`**: Trust forever. Solo re-validar si user pide.

### 8.4 Backup y recovery

Antes de cada update del TOML, se renombra el viejo a `memory.bak`:

```cpp
void ConfigStore::save_atomic(const MemoryData& data) noexcept {
    write_to(temp_path);    // memory.toml.tmp
    rename(real_path, backup_path);   // memory.toml → memory.bak
    rename(temp_path, real_path);     // memory.toml.tmp → memory.toml
}
```

Si el load del actual falla (corrupto), fallback al backup.

### 8.5 Reset y export

UI debe exponer:
- "Reset all learnings" → borra memory.toml completo.
- "Reset learning for X.exe" → borra UN entry.
- "Export learnings" → copia memory.toml para sharing.
- "Import learnings" → merge from sibling memory.toml, conflict resolution:
  el más reciente gana.

---

## §9 — UX: transparencia y reversibility

### 9.1 Action audit trail

Toda acción se registra antes de ser aplicada:

```cpp
struct ActionAuditEntry {
    uint64_t tsc_planned;       // cuando se decidió
    uint64_t tsc_applied;       // 0 si nunca se aplicó
    uint64_t tsc_reverted;      // 0 si sigue activa
    uint32_t target_pid;
    char     target_exe[40];
    uint32_t rule_id;
    uint8_t  mode;              // Auto / Assist / Manual
    uint8_t  result;            // Success / PermDenied / TargetGone / UserDeclined
    uint8_t  _pad[2];
    uint64_t prev_affinity_mask;
    uint64_t new_affinity_mask;
    char     human_readable[64];
};
static_assert(sizeof(ActionAuditEntry) == 128);
```

Persistido en `%LOCALAPPDATA%\Ayama\audit.bin` (binary, append-only).

UI tab "Actions" lo muestra en orden inverso (más reciente primero) con
campos: timestamp, exe, rule, "what changed in plain English", revert button.

### 9.2 Plain-English descriptions

Cada `ActionAuditEntry::human_readable` es generada cuando se planea la acción:

```cpp
inline void describe_action(const PolicyDecision& d,
                            const TargetProcess& t,
                            char* out, uint32_t out_sz) noexcept {
    switch (static_cast<ActionKind>(d.action_kind)) {
        case ActionKind::PinAffinity: {
            const uint32_t n_cores = popcount(d.core_mask);
            std::snprintf(out, out_sz,
                "Pinned %s to %u cores (V-Cache CCD)",
                t.name, n_cores);
            break;
        }
        case ActionKind::SetPriority: {
            std::snprintf(out, out_sz,
                "Raised %s priority to %s",
                t.name, priority_name(d.priority_class));
            break;
        }
        case ActionKind::Revert: {
            std::snprintf(out, out_sz, "Restored default settings for %s", t.name);
            break;
        }
        default: break;
    }
}
```

### 9.3 "Big red button": global revert

UI siempre tiene un botón visible "Revert All" + hotkey global `Ctrl+Alt+R`:

```cpp
class GlobalRevertHotkey {
public:
    void install() noexcept {
#ifdef _WIN32
        RegisterHotKey(NULL, 1, MOD_CONTROL | MOD_ALT, 'R');
        // Mensaje WM_HOTKEY se procesa en el message loop.
#endif
    }

    void on_triggered() noexcept {
        executor_.revert_all();
        notify_tray("All Ayama actions reverted");
    }
};
```

Esto es la "salida de emergencia" que mata cualquier preocupación del usuario.

### 9.4 Dry-run mode

Modo Assist usa dry-run por debajo:

```cpp
struct DryRunResult {
    PolicyDecision  proposed;
    char            description[128];
    uint64_t        prev_affinity_mask;
};

class ActionExecutor {
public:
    // Real apply.
    [[nodiscard]] std::expected<void, gma::Error>
    apply(const PolicyDecision& d) noexcept;

    // What would apply do? Return the description without acting.
    [[nodiscard]] DryRunResult
    dry_run(const PolicyDecision& d) noexcept;
};
```

UI usa `dry_run` para popular "Apply?" dialog en modo Assist.

---

## §10 — Robustez: error handling y degradación

### 10.1 Tabla de degradación

| Subsistema falló | Síntoma | Degradación |
|---|---|---|
| ETW (no admin) | `start()` returns `PermissionDenied` | Fallback a NT polling cada 500ms |
| CrossProcessAffinity (no perms) | `apply()` returns `PermissionDenied` | Mostrar warning, no aplicar; sugerir "Run as admin" |
| Shared memory failed | `connect()` returns `Unavailable` | UI muestra "Agent not running, please start" |
| Memory.toml corrupto | parse error | Cargar memory.bak; si también corrupto, defaults |
| HardwareTopology probe failed | rara, casi nunca pasa | Modo "no policy" — Ayama solo observa |
| WindowsTuner failed | esperable sin admin | Ignorar; reportar en UI |

### 10.2 Error en hot path: nunca crashea

Cualquier error en tick: se logguea y se continúa. NO se aborta el proceso.

```cpp
void AgentRuntime::tick() noexcept {
    try {
        observer_.refresh();
    } catch (...) {
        // Esto NO debería pasar (todo es noexcept).
        // Si pasa, contar error y continuar.
        ++hot_path_exceptions_;
    }

    auto sample_result = metrics_.sample(...);   // std::expected
    if (!sample_result) {
        ++sample_errors_;
        // No abort, continue with stale metrics.
    }

    // ... el tick siempre completa.
}
```

`hot_path_exceptions_` y `sample_errors_` son visibles en la UI panel "Ayama"
para debug.

### 10.3 Watchdog interno

Ayama-agent tiene un watchdog thread que monitorea al main:

```cpp
class InternalWatchdog {
    std::atomic<uint64_t> last_tick_tsc_{0};
    std::thread           watcher_;
    std::atomic<bool>     stop_{false};

public:
    void start() noexcept {
        watcher_ = std::thread([this]() noexcept {
            while (!stop_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                const auto now = rdtsc();
                const auto last = last_tick_tsc_.load(std::memory_order_acquire);
                const auto elapsed_ms = (now - last) * 1000ull / tsc_freq();
                if (elapsed_ms > 10'000) {
                    // Main thread stuck 10s+.
                    log_critical("Main thread stalled — initiating recovery");
                    request_recovery();
                }
            }
        });
    }
};
```

`request_recovery` puede: revert all actions, log to disk, restart main loop.

### 10.4 Single-instance enforcement

Solo una instancia de Ayama-agent puede correr:

```cpp
class SingleInstance {
public:
    [[nodiscard]] bool acquire() noexcept {
#ifdef _WIN32
        mutex_ = CreateMutexW(NULL, FALSE, L"Local\\AyamaAgentMutex.v1");
        if (mutex_ == NULL) return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mutex_);
            return false;
        }
#endif
        return true;
    }

    ~SingleInstance() noexcept {
        if (mutex_) CloseHandle(mutex_);
    }
};
```

En main:
```cpp
SingleInstance inst;
if (!inst.acquire()) {
    fprintf(stderr, "Ayama agent already running. Exiting.\n");
    return 1;
}
```

---

## §11 — Compatibilidad: cero invasive operations

### 11.1 Operaciones PERMITIDAS

Estas son consideradas legales por la mayoría de los anticheats (consultar
docs individuales). **Las operaciones invasivas que Ayama necesita ya están
encapsuladas en Gamma APIs portables** — no se llama directamente al Win32 API:

- **`gma::hw::set_process_affinity(pid, mask)`** (FR-3) — bajo el hood usa
  `SetProcessAffinityMask`. Process Lasso y otros tools comerciales lo usan;
  aceptado.
- **`gma::hw::set_process_priority(pid, class)`** (FR-9) — bajo el hood usa
  `SetPriorityClass`. Aceptado.
- **`gma::proc::ProcessMetricsSnapshot::capture()`** (FR-11) — bajo el hood usa
  `NtQuerySystemInformation(SystemProcessInformation)`. **No abre handles
  per-PID** — más limpio para anticheats que el patrón legacy
  `OpenProcess(PROCESS_QUERY_INFORMATION)`.
- **`gma::proc::enumerate_processes()`** (FR-4) — usa `EnumProcesses` /
  `Toolhelp32Snapshot`. Read-only.
- **`gma::etw::SessionManager`** (FR-6) — ETW kernel providers (system-wide,
  no per-process injection). Aceptado.
- Performance counters API (PDH) — usado directamente (no Gamma wrapper aún).
- `EnumProcessModules` — listar DLLs cargadas (read-only de PEB), para
  classifier heurístico.

### 11.2 Operaciones PROHIBIDAS (red lines)

Ninguna de estas operaciones se ejecutará en NINGÚN scenario de Ayama:

- ❌ `WriteProcessMemory` — modificación de memoria de otros procesos.
- ❌ `ReadProcessMemory` — lectura de memoria de otros procesos.
- ❌ DLL injection (CreateRemoteThread, SetWindowsHookEx para DLLs, AppInit).
- ❌ Hooking (Detours, EasyHook, IAT hooking).
- ❌ Driver kernel-level access — Ayama es 100% user-mode.
- ❌ Modificación de archivos del juego.
- ❌ Bypass de DRM o cualquier protection layer.

Cumplir estas reglas asegura que Ayama no puede ser confundido con un cheat,
incluso por anticheats agresivos. Es OK ser detectado por anticheat — lo
que NO es OK es ser banneado por tampering.

### 11.3 Documentación pública

Ayama publica en su website (o README):

> "Ayama uses only documented Windows APIs that are publicly available to any
> userspace application: SetProcessAffinityMask, SetPriorityClass, and ETW
> performance counters. Ayama does NOT inject DLLs, modify game memory,
> hook game functions, or use kernel drivers. The same operations are used
> by tools like Process Lasso, Process Hacker, and the Windows Task Manager."

Esto da al usuario información para evaluar el riesgo en anticheats.

### 11.4 Anticheat compatibility matrix (actualizar con datos reales)

| Anticheat | SetProcessAffinity | SetPriorityClass | OpenProcess (read-only) | Veredicto preliminar |
|---|---|---|---|---|
| EAC | ✓ permitido (verified Process Lasso) | ✓ | ⚠ may flag | Probar después de Phase 1 |
| BattlEye | ✓ permitido | ✓ | ⚠ may flag | Probar después de Phase 1 |
| Vanguard (Riot) | ⚠ kernel anticheat — mayor escrutinio | ⚠ | ⚠ | Investigar separately |
| FACEIT | ⚠ extremely strict | ⚠ | ⚠ | Última prioridad |

**Acción:** investigar cada anticheat tier por separado en Phase 2.

---

## §12 — Templates de código reusable

### 12.1 Skeleton de un Bloque Ayama

```cpp
// ayama/include/ayama/<sub>/MyComponent.hpp
// MyComponent — brief description.
//
// Detailed description (3-5 lines).
//
// Threading: <X-thread / multi-thread / single-thread-only>.
// Resource: <CPU class, RAM class>.
// Privilege: <None / Admin>.
//
// §Block <X.Y>, ayama::<sub>
#pragma once
#include <gamma/schema/Error.hpp>
#include <cstdint>
#include <expected>

namespace ayama::<sub> {

class MyComponent {
public:
    static constexpr uint32_t kMaxFoo = 32u;

    MyComponent() noexcept;
    ~MyComponent() noexcept;

    MyComponent(MyComponent const&)            = delete;
    MyComponent& operator=(MyComponent const&) = delete;
    MyComponent(MyComponent&&)                 = delete;
    MyComponent& operator=(MyComponent&&)      = delete;

    [[nodiscard]] std::expected<void, gma::Error> start() noexcept;
    void stop() noexcept;

    // Hot path methods: noexcept, no alloc.
    void on_tick() noexcept;

private:
    bool started_{false};
    // Pre-allocated buffers go here.
};

} // namespace ayama::<sub>
```

### 12.2 Test skeleton

```cpp
// ayama/tests/<sub>/my_component_test.cpp
#include <ayama/<sub>/MyComponent.hpp>
#include <cassert>
#include <cstdio>

int main() {
    using namespace ayama::<sub>;

    MyComponent c;
    auto r = c.start();
    if (!r) {
        std::fprintf(stderr, "start failed: code=%d\n",
                     static_cast<int>(r.error().code));
        return 1;
    }

    // Test body
    for (int i = 0; i < 100; ++i) {
        c.on_tick();
    }

    c.stop();
    std::printf("OK\n");
    return 0;
}
```

### 12.3 UI panel skeleton (template reusable)

```cpp
// examples/standard_window/widgets/ayama_<panel>_panel.hpp
// Ayama <panel> panel — short description.
//
// Pattern:
//   inline void draw_ayama_<panel>_panel(const AppState& s,
//                                          const ayama::ipc::AyamaClient* ac) noexcept;
//
//   - Reads `s` for general AppState fields (hardware, behavior, etc.)
//   - Reads from `ac` for live Ayama-agent data.
//   - If `ac == nullptr || !ac->is_connected()`: shows graceful fallback.
//   - Zero dynamic allocation in draw loop.
//
// §Block 4.X, ayama UI template
#pragma once
#include "../AppState.hpp"
#include <ayama/ipc/AyamaClient.hpp>
#include <imgui.h>

inline void draw_ayama_<panel>_panel(const AppState& s,
                                       const ayama::ipc::AyamaClient* ac) noexcept
{
    ImGui::Spacing();

    if (ac == nullptr || !ac->is_connected()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.75f, 0.20f, 1.f});
        ImGui::TextWrapped(
            "Ayama agent is not running. Launch ayama-agent.exe to "
            "enable optimization features.");
        ImGui::PopStyleColor();
        return;
    }

    // Real content (panel-specific)
    // ...
}
```

### 12.4 CMakeLists.txt pattern para Ayama subcomponente

```cmake
# ayama/<sub>/CMakeLists.txt
# ayama_<sub> — brief description
#
# Dependencies: <listed>
#
# §Block <X.Y>

add_library(ayama_<sub> STATIC
    src/MyComponent.cpp
)

target_include_directories(ayama_<sub> PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ayama_<sub> PUBLIC
    gamma_schema
    # ... other gamma deps
)

target_compile_features(ayama_<sub> PRIVATE cxx_std_23)

if(MSVC)
    target_compile_options(ayama_<sub> PRIVATE /W4 /WX)
else()
    target_compile_options(ayama_<sub> PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Tests
if(GAMMA_BUILD_TESTS)
    add_executable(ayama_<sub>_test tests/my_component_test.cpp)
    target_link_libraries(ayama_<sub>_test PRIVATE ayama_<sub>)
    add_test(NAME ayama_<sub>_test COMMAND ayama_<sub>_test)
endif()
```

---

## §13 — Pilares nuevos (Gamma 1.1.0): process / ipc / etw

Esta sección documenta los patrones de uso de los tres pilares creados por el
FR pack v1.1.0 (mayo 2026). Estos son los **primeros candidatos** para reemplazar
código Ayama hand-rolled.

### 13.1 `gamma_process` — bulk metrics y self-identity

**Headers que importan:**
```cpp
#include <gamma/process/ProcessEnumerator.hpp>       // FR-4
#include <gamma/process/ProcessMetricsSnapshot.hpp>  // FR-11
#include <gamma/process/CurrentProcess.hpp>          // FR-18
```

**Patrón A — sampling de targets:**
```cpp
class MetricsCollector {
    gma::proc::ProcessMetricsSnapshot snapshot_;

public:
    std::expected<void, gma::Error> start() noexcept {
        auto r = gma::proc::ProcessMetricsSnapshot::create();
        if (!r) return std::unexpected(r.error());
        snapshot_ = std::move(*r);
        return {};
    }

    void sample(const uint32_t* pids, uint32_t n,
                TargetMetrics* out) noexcept {
        (void)snapshot_.capture();   // ~150 µs, una syscall
        gma::proc::ProcessMetrics buf[32];
        snapshot_.extract(pids, n, buf);

        for (uint32_t i = 0; i < n; ++i) {
            out[i].pid                = buf[i].pid;
            out[i].cpu_usage_pct      = compute_cpu_pct(buf[i], prev_[i]);
            out[i].working_set_bytes  = buf[i].working_set_bytes;
            // ...
        }
    }
};
```

**Patrón B — self-identity en logging y SHM:**
```cpp
void initialize_shm() noexcept {
    region_.header().agent_pid.store(gma::proc::self_pid(),
                                      std::memory_order_release);

    std::fprintf(log_, "[Ayama] Started: pid=%u name=%s\n",
                 gma::proc::self_pid(), gma::proc::self_name());
}
```

**Patrón C — enumeración filtrada por nombre:**
```cpp
void ProcessObserver::refresh() noexcept {
    gma::proc::ProcessEntry entries[256];
    const uint32_t n = gma::proc::enumerate_processes(entries, 256);

    n_targets_ = 0;
    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t p = 0; p < n_patterns_; ++p) {
            if (case_insensitive_match(entries[i].name, patterns_[p])) {
                targets_[n_targets_++] = make_target(entries[i]);
                if (n_targets_ >= kMaxTargets) return;
                break;
            }
        }
    }
}
```

### 13.2 `gamma_ipc` — ring + shm header-only

**Headers que importan:**
```cpp
#include <gamma/ipc/Ring.hpp>       // FR-12
#include <gamma/ipc/ShmRegion.hpp>  // FR-5
```

**Patrón D — Ring producer/consumer entre threads:**

Ver §1.5 (Ring buffers) y §2.3 (Action log ring).

**Patrón E — ShmRegion seqlock publish:**

Ver §2.1 (SHM protocol).

**Combinación con caveat — ring en SHM:**

⚠ **Hallazgo arquitectural (mayo 2026):** `gma::ipc::Ring<T,Cap>` tiene constructors
de copia/movimiento `= delete` (decisión deliberada para prevenir copias accidentales con
atomics que se desincronizarían). Esto significa que NO es `std::is_trivially_copyable`, y
**no puede ser campo de un `Payload` de `ShmRegion`** porque `ShmRegion` exige
`is_trivially_copyable<Payload>` en compile-time.

**Patrones válidos para `Ring` en SHM (mayo 2026):**

**Opción 1 — Ring directo en SHM sin ShmRegion wrapper** (lo que Ayama hace hoy):
```cpp
// Layout SHM mapeado manualmente (CreateFileMappingW / mmap).
// Producer y consumer acceden a través del puntero mapeado, ring vive in-place.
struct AyamaShmLayout {
    AyamaShmHeader header;       // magic + version + seq + agent_pid
    AyamaStateHeader state;
    TargetProcess    targets[32];
    /* ... */
    gma::ipc::Ring<ActionLogEntry, 128> action_log;  // accedido in-place
};
```

**Opción 2 — Dos `ShmRegion` separados** (para uso futuro al cerrar Master Plan §11.1):
```cpp
// ShmRegion 1: state + arrays (Payload trivially_copyable)
auto state_region = gma::ipc::ShmRegion<AyamaShmHeader, AyamaStatePayload>::create(
    "Local\\AyamaAgent.v1.state", kAyamaMagic, 1u);

// ShmRegion 2: action log ring (Payload = Ring wrapper trivially_copyable, o usar otro mecanismo)
// Pendiente: cambio en gma::ipc::Ring o wrapper trivially_copyable
```

**Opción 3 — Cambiar `gma::ipc::Ring` (FR a Gamma)**:
Hacer copy/move ctors `= default` con un comment explícito "for SHM-payload use only;
do not copy in regular code". Trade-off: facilita SHM embedding, pero abre puerta a
copias accidentales que crearían atomics desincronizados.

**Nota técnica:** cuando `Ring` vive en SHM, los atomics SÍ son inter-process safe
(C++20 `std::atomic<uint64_t>` con lock-free operations cumple en x86_64) — el issue
es solo de tipos C++ (`is_trivially_copyable`), no de semántica runtime.

### 13.3 `gamma_etw` — sesión + consumer thread

**Header que importa:**
```cpp
#include <gamma/etw/SessionManager.hpp>   // FR-6
```

Ver §2.4 (ETW callbacks) y §5 (ETW patterns) para uso completo.

**Provider GUIDs disponibles** (no más magic numbers):
```cpp
namespace gma::etw::providers {
    extern constexpr GUID kKernelProcess;
    extern constexpr GUID kKernelThread;
    extern constexpr GUID kKernelContextSwitch;
    extern constexpr GUID kKernelDpc;
    extern constexpr GUID kKernelHardFault;
    extern constexpr GUID kDxgi;            // PresentMon dependency
    // ...
}
```

### 13.4 Path de migración desde código Ayama legacy

Orden de migración con **estado verificado contra código (mayo 2026)**.

| # | Migración | Estado | Test que lo verifica |
|---|-----------|--------|----------------------|
| 1 | `ActionLogRing` → `gma::ipc::Ring<ActionLogEntry, 128>` (FR-12) | ✅ DONE | `ayama_action_test`, `ring_test` |
| 2 | `MetricsCollector` per-PID loop → `ProcessMetricsSnapshot::capture()+extract()` (FR-11) | ✅ DONE | `ayama_core_test` (cubre integración) |
| 3 | `AgentRuntime` wake event → `gma::hal::WakeEvent` (FR-16) | ✅ DONE | `ayama_core_test`, `wake_event_test` |
| 4 | `apply_memory_budget` → `gma::tuning::set_self_working_set()` (FR-19) | ✅ DONE | `working_set_test` |
| 5 | `GetCurrentProcessId/getpid` → `gma::proc::self_pid()` (FR-18) | ✅ DONE | `current_process_test` |
| 6 | `SelfMonitor` RSS via `GetProcessMemoryInfo` → `gma::tuning::get_self_working_set()` (FR-19) | ✅ DONE | `working_set_test` |
| 7 | `AyamaAgentPublisher`/`AyamaClient` SHM hand-rolled → `gma::ipc::ShmRegion<H,P>` (FR-5) | ❌ **PENDIENTE** — ver Master Plan §11.1 | (futuro) `ayama_ipc_roundtrip_test` |
| 8 | `MetricsCollector` ETW callback (stub) → `gma::etw::SessionManager` real hookup (FR-6) | ✅ **DONE** (mayo 2026) — cableado en `MetricsCollector_win32.cpp`. Pendiente: verificación bajo admin + TID→PID (FR-13) | (futuro) `ayama_etw_smoke_test` |

**Reglas durante la migración:**

- **No migrar todo de golpe.** Hacer un componente a la vez, con `ctest --output-on-failure` PASS entre cada paso. Hoy `43/43` tests PASS — cualquier regresión debe ser caught antes del commit siguiente.
- **El código gana sobre los docs.** Si una afirmación en este doc o en `AYAMA_MASTER_PLAN.md` no concuerda con el header en `pillars/...`, actualizar el doc, no el código.

### 13.5 APIs Ayama que se vuelven thin wrappers

Tabla actualizada con estado real. "LOC actual" se mide del código a fecha mayo 2026; "LOC objetivo" es la meta post-migración.

| Componente | LOC actual | LOC objetivo | Esencia | Estado |
|------------|-----------:|-------------:|---------|--------|
| `ActionLogRing` | 0 (typedef) | 0 | `using ActionLogRing = gma::ipc::Ring<ActionLogEntry, 128>;` | ✅ done |
| `AyamaAgentPublisher` | ~247 | ~50 | thin wrapper sobre `ShmRegion::create` + `begin_write()` RAII | ❌ todavía hand-rolled |
| `AyamaClient` | ~95 | ~40 | thin wrapper sobre `ShmRegion::open` + `try_read_consistent` | ❌ todavía hand-rolled |
| `MetricsCollector::sample` | ~80 | ~80 | drive `ProcessMetricsSnapshot` + transform (FR-11 hecho) | ✅ done |
| `MetricsCollector::start` ETW path | ~50 (stub) | ~40 | configure `gma::etw::SessionManager` providers | ❌ pendiente — el callback es stub vacío |
| `AgentRuntime::wake_*` ops | ~10 | ~10 | `WakeEvent.signal()` / `wait()` | ✅ done |
| `SelfMonitor` RSS | ~12 | ~6 | `gma::tuning::get_self_working_set(&cur, &peak)` | ✅ done |

**Total LOC ahorrado hoy: ~600** (de un estimado original de ~1070).
**LOC adicionales por ahorrar al cerrar §11.1 + §11.2: ~430.**

---

## Anexo A — Checklist al implementar un Bloque

Antes de declarar un Bloque "completo", verificar:

- [ ] Header tiene comment-block inicial con descripción + threading + resource + §
- [ ] Todas las constantes `kFoo` están definidas en compile-time.
- [ ] No hay `std::vector` en clase miembro (usar `std::array<T, N>`).
- [ ] No hay `new`/`malloc` después de constructor.
- [ ] Métodos del hot path son `noexcept`.
- [ ] Errors retornan `std::expected<T, gma::Error>`.
- [ ] Tests cubren: success, permission denied, target gone, edge cases.
- [ ] Tests miden el resource budget (CPU%, RAM) y fallan si excede.
- [ ] Documentación de privilegios al inicio del header.
- [ ] Static_assert para POD types: trivially_copyable + standard_layout + size <= 4096.
- [ ] Si hay state persistido: load tiene fallback a defaults, save es atomic.
- [ ] Si hay IPC: usa patrón seqlock o lock-free establecido en §2.
- [ ] Si hay self-monitoring: registra a SelfMonitor.
- [ ] Si hay ETW: drena explícitamente cada tick.
- [ ] UI panel (si aplica): maneja `ac == nullptr` elegantemente.

---

**Fin del Implementation Strategies v0.2**

Última actualización: Mayo 2026 (post FR pack v1.1.0)
Próxima revisión: después de FR-13/14/15/17 (los 4 FRs en planeación para Phase 2)
  — esos requerirán updates a §5 (ETW), §7 (foreground detection), §8 (TOML),
  y un §17 (DedicatedThread patterns).
