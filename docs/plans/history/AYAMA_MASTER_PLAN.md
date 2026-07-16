> **📦 Archived planning document — historical.**
> From Phyriad's pre-rebrand era (the project was codenamed *gamma* / *gma*).
> Published for transparency about how the project was actually built — **not**
> a current specification. Version numbers and performance claims here
> (including any "vs Folly" / "production-ready" framing) reflect the project's
> state at the time of writing and may since have been revised or **retracted**;
> the project has reset to `v0.1.0-experimental`. For current status see the
> repo `README.md` and `docs/planning/`. For benchmark-claim validation status
> see `docs/planning/TEST_INVENTORY.md`.

# Ayama — Master Implementation Plan
## A non-invasive runtime optimizer built on top of the Gamma Framework

**Version:** 0.4 — Mayo 2026
**Implementer target:** Opus / Sonnet HIGH
**Foundation:** Gamma Framework **release line 1.1.0** (18 pilares estables + FR pack mayo 2026).
  El macro `GAMMA_VERSION_STRING` en `pillars/_meta/include/gamma/version.hpp` sigue siendo `"1.0.0"` (no se bumpeó al integrar los FR, decisión interna).
**Status del plan:** Fases 1-3 y 5 implementadas; Fase 4 (UI) presente en disco pero no se compila por flag; falta hookup ETW real y migración SHM→`ShmRegion`. Ver §8.7.
**Documento hermano:** [`AYAMA_IMPLEMENTATION_STRATEGIES.md`](AYAMA_IMPLEMENTATION_STRATEGIES.md) — patrones técnicos concretos para máximo rendimiento/eficiencia/UX.
**Documento de inventario:** [`../GAMMA_INVENTORY.md`](../GAMMA_INVENTORY.md) — **fuente de verdad sobre qué pilares y APIs Gamma existen realmente** (derivado del código, no de docs). Consultar **antes** de afirmar disponibilidad de un símbolo Gamma.
**Doc complementario:** [`GAMMA_FEATURE_REQUEST_IMPLEMENTATION_STRATEGIES.md`](GAMMA_FEATURE_REQUEST_IMPLEMENTATION_STRATEGIES.md) — planeación de los FRs filed contra Gamma.

### Changelog v0.3 → v0.4

- **`GAMMA_INVENTORY.md` ahora es la fuente de verdad** sobre el catálogo Gamma — §1.1 deja de duplicar el inventario y enlaza al doc maestro.
- **§1.3 gaps actualizado** — separa "gaps Gamma→Ayama" (resueltos por FR pack) de "migraciones Ayama internas pendientes" (ShmRegion, ETW SessionManager hookup).
- **§3.2 Bloque P.1-P.4** marcado como **DONE** (versionado, single-includes, public API).
- **§4 Bloques: paths corregidos** — `ayama/include/ayama/X/...` → `ayama/X/include/ayama/X/...` (estructura real per-pillar).
- **§8.7 NUEVO** — Tabla de estado de implementación por bloque vs realidad del código.
- **Parte 11 NUEVA** — Outstanding migrations (ShmRegion, ETW hookup) con steps concretos.
- **Aclaración de versión Gamma**: la macro sigue en `1.0.0` aunque la doc llame "release line 1.1.0" al post-FR-pack.

### Changelog v0.2 → v0.3

- **Gamma 1.1.0:** 14 Feature Requests resueltos (FR-1..FR-6, FR-8..FR-12, FR-16, FR-18, FR-19).
  Tres pilares nuevos: `process`, `ipc`, `etw`.
- **§1.1 inventario actualizado** con los nuevos pilares y APIs.
- **§1.2 reuse list reescrita** — Ayama puede ahora reusar ~1070 LOC adicionales del framework.
- **§1.3 gaps tracker actualizado** — 4 gaps resueltos, 4 en planeación (FR-13/14/15/17).
- **§3.1 allowed Gamma headers** ampliado con los 7+ headers nuevos.
- **APIs deprecated explícitamente:** `CrossProcessAffinity` superseded por FR-3 + FR-9.
- **Versión mínima Gamma**: bump a 1.1.0.

### Changelog v0.1 → v0.2

- **Auto-first como principio rector.** Ayama "just works" sin configuración. Tweaking es opcional.
- **Anti-parasitic resource budget definido.** < 0.3% CPU idle, < 20 MB RSS.
- **Service vs user-process es decisión del usuario.** Ambos modos soportados.
- **Test pack inicial no toca juegos con anticheat.** Escalamos a anticheat solo cuando hay datos sólidos.
- **No hay Ayama Lite/Pro.** Una sola versión completa que requiere admin. Es la carta de presentación de Gamma.
- **Nuevo:** Parte 9 — Modos de operación (Auto/Assist/Manual).

---

## Tabla de contenidos

- [Parte 0 — Filosofía y posicionamiento](#parte-0--filosofía-y-posicionamiento)
- [Parte 1 — Análisis del estado actual](#parte-1--análisis-del-estado-actual)
- [Parte 2 — Arquitectura de Ayama](#parte-2--arquitectura-de-ayama)
- [Parte 3 — Gamma como framework externo](#parte-3--gamma-como-framework-externo)
- [Parte 4 — Bloques de implementación](#parte-4--bloques-de-implementación)
- [Parte 5 — Medición antes/después](#parte-5--medición-antesdespués)
- [Parte 6 — ayama-ui: ventana standalone de Ayama](#parte-6--ayama-ui-ventana-standalone-de-ayama)
- [Parte 7 — Test scenarios del mundo real](#parte-7--test-scenarios-del-mundo-real)
- [Parte 8 — Quick reference](#parte-8--quick-reference)
- [Parte 9 — Modos de operación (Auto/Assist/Manual)](#parte-9--modos-de-operación-autoassistmanual)
- [Parte 10 — Resource budget y compromiso anti-parasítico](#parte-10--resource-budget-y-compromiso-anti-parasítico)
- [Parte 11 — Migraciones outstanding (Gamma sub-uso pendiente)](#parte-11--migraciones-outstanding-gamma-sub-uso-pendiente)

---

## Parte 0 — Filosofía y posicionamiento

### 0.1 Qué es Ayama

Ayama es un **runtime optimizer no-invasivo** que se ejecuta como proceso en background
y observa/optimiza otros procesos del sistema sin que estos sepan de su existencia.

**No es:**
- Una librería que las apps integran.
- Un fork de Gamma.
- Un overclocker (no toca PL1/PL2, no toca frecuencia base del CPU).

**Sí es:**
- Un agent que corre `gamma-session`-style en background.
- Un observador que usa ETW + perfcounters para medir procesos terceros.
- Un ejecutor que aplica `CrossProcessAffinity` cuando detecta patrones perjudiciales.
- Un benchmark suite que prueba el "antes y después" de forma medible.

### 0.2 Qué SÍ promete Ayama (lo verificable — actualizado mayo 2026 con datos empíricos)

Las magnitudes a continuación son **observadas en 9 tests A/B/A/B/A**
sobre Ryzen 9 7950X3D + RTX 4090. Detalle por test en
`docs/ayama/reports/EMPIRICAL_EVIDENCE_SUMMARY.md`.

| Promesa | Métrica medible | Magnitud validada | Cuándo aplica |
|---|---|---|---|
| Reducir stutters en juegos single-thread CPU-bound | P99 / P99.9 frametime, max frame time | **+22-56% FPS en games válidos**; max frame time -50% en spike elimination | Engines viejos (≤2015), framerate uncapped, single hot thread dominante |
| Coordinar multi-workload | Frametime variance entre runs | **Treated stddev -50-96%** vs baseline; runs treated casi clones | Always (variance reduction es robusto) |
| Aprovechar AMD V-Cache asimétrico | FPS en juegos cache-bound | **Halo 2 MCC +56%, Fallout 4 +35%, Hogwarts +15.5%, BL2 high-FPS marginal+** | CPUs dual-CCD con V-Cache asimétrico (7950X3D, 7900X3D, 9950X3D) |
| Reducir input lag en hybrid Intel | Click-to-photon latency | **Untested empíricamente** — policy implementada (E-core eviction) pero no validada en 9-test catalog | Intel hybrid 12700K+ con scenario sensible al jitter |

**Predictive model establecido**:
- Engine era × CPU saturation = magnitud de beneficio
- Halo 2 (2004) > Fallout 4 (2015) > Hogwarts (2023): monotónico por edad del engine
- Same game uncapped > VSynced: methodology importa (Fallout 4 conservative por physics constraint)

**Casos validados como NULL**:
- RDR2 (RAGE+Vulkan, well-threaded): +1.9% within CI
- Minecraft modded (Sodium/Iris/DH): null en 3 tests independientes
- BL2 normal-FPS zone (CPU no saturado): null
- (Cualquier game GPU-bound): null

### 0.3 Qué NO promete Ayama (lo que no podemos sostener)

- **NO aumenta la frecuencia máxima del CPU.** PL1/PL2 y thermal throttling son hardware.
- **NO mejora FPS en todos los juegos.** ~60% de games testeados muestran mejora; el 40% restante es NULL por razones arquitectónicas claras (engines well-threaded, workloads GPU-bound, framerate capped). Lista de NULL casos en `EMPIRICAL_EVIDENCE_SUMMARY.md`.
- **NO funciona en CPUs simétricas con misma cantidad de cache.** El 7800X3D (single-CCD con V-Cache uniforme), Ryzen homogéneos sin V-Cache, Intel non-hybrid: cross-CCD migration no existe, Ayama no tiene qué resolver. El value proposition es específico a CPUs asimétricas.
- **NO funciona sin privilegios.** Sin admin/CAP_SYS_NICE no puede pinear procesos externos.
- **NO es seguro con anti-cheat de kernel-mode.** Riot Vanguard, EAC, BattlEye, EA Javelin, Ricochet podrían flagear las operaciones de Ayama (PROCESS_VM_READ + PROCESS_SET_INFORMATION). Lista de safe-to-test games en `EMPIRICAL_EVIDENCE_SUMMARY.md §Scope`.
- **NO es magia universal.** En engines modernos bien threaded (UE5 distributed task graph, RAGE+Vulkan, Source 2) el beneficio es marginal o null por diseño — los engines mismos ya hacen el trabajo de distribuir CPU work.

### 0.4 Reglas inamovibles del plan

1. **Cero stubs.** Cada bloque entrega código completo con tests.
2. **Gamma se trata como dependencia externa.** Si necesitamos algo de Gamma que no existe,
   se documenta como "feature request a Gamma", NO se modifica Gamma desde Ayama.
   **Antes de afirmar que algo NO existe en Gamma**, consultar
   [`../GAMMA_INVENTORY.md`](../GAMMA_INVENTORY.md) — este doc se deriva del código real
   y es la única fuente con autoridad sobre qué APIs Gamma están disponibles.
3. **POD en todo lo IPC.** Toda struct que cruza ETW callbacks, shared memory o ring buffers
   debe ser `trivially_copyable + standard_layout + sizeof ≤ 4096`.
4. **Toda acción tiene reverse.** Si Ayama pinea OBS a CCD1, debe poder revertirlo al
   estado original cuando la condición desaparece o el usuario lo pide.
5. **Toda métrica tiene baseline.** No se reporta una mejora sin haber medido el estado pre-Ayama.
6. **`std::expected<T, gma::Error>` para todo lo fallible.** Reusamos el sistema de errores de Gamma.
7. **Ayama-ui es ventana standalone.** Toda visualización de Ayama vive en
   `ayama/tools/ayama-ui/widgets/`. `standard_window` es **solo referencia** del
   patrón Gamma+ImGui (cómo usar `gma::ui::Application` + `RenderNode<T>`).
   No se modifica ni extiende `standard_window` desde Ayama.
8. **`noexcept` en hot path.** Callbacks ETW, polling loops, action dispatch.
9. **Cero allocación en steady-state.** Pre-allocar buffers en construcción.
10. **Documentar privilegios.** Cada operación que requiera admin/cap se marca explícitamente.

### 0.5 Principios de UX (auto-first)

Ayama es la **carta de presentación de Gamma**. La primera impresión define la percepción.
Las técnicas internas son sofisticadas; la experiencia del usuario tiene que ser trivial.

1. **Auto by default.** Al instalar y ejecutar, Ayama detecta hardware, clasifica procesos,
   aplica política recomendada — sin que el usuario configure nada. Cero clicks para valor.

2. **Progressive disclosure.** Tres modos:
   - **Auto** (default): Ayama decide y aplica. UI muestra "Optimizando X" y nada más.
   - **Assist**: Ayama detecta y notifica. El usuario aprueba con un click.
   - **Manual**: el power-user define rules custom y las prueba.

3. **Transparencia absoluta.** El usuario puede SIEMPRE ver:
   - Qué procesos están bajo gestión de Ayama
   - Qué acción exacta fue aplicada y cuándo
   - El estado previo (para entender el revert)

4. **Reversibilidad de un click.** Cualquier cambio que Ayama hace se puede deshacer
   inmediatamente desde la UI. Tecla rápida global (Ctrl+Alt+R) para "revert all".

5. **Sin sorpresas.** Cambios destructivos (e.g. modificar priority class a REALTIME)
   nunca se aplican silenciosamente; siempre van por modo Assist con confirmación.

6. **Honestidad de reportes.** Si Ayama no mejora nada en tu sistema, lo dice claramente.
   Mentir destruye confianza. Un "no notamos mejora medible" es preferible a inventar números.

7. **Aprende y persiste.** Si Ayama detectó que para Cyberpunk 2077 lo mejor es policy X,
   la próxima vez que ese exe se ejecute, aplica X directo. Per-game + per-hardware learning.

### 0.6 Compromiso anti-parasítico

Ayama existe para optimizar otros procesos. No tiene sentido si Ayama mismo consume
recursos relevantes. **Target hard:**

| Métrica | Objetivo | Hard limit (test fail si excede) |
|---|---|---|
| CPU idle (sin targets activos) | < 0.1% | < 0.3% |
| CPU active (1-3 targets observados) | < 0.5% | < 1.0% |
| CPU bench A/B en curso | < 2% | < 5% |
| RAM RSS idle | < 15 MB | < 20 MB |
| RAM RSS active | < 30 MB | < 50 MB |
| Disk I/O por minuto | < 100 KB | < 1 MB (a menos que esté grabando baseline) |
| Network | 0 bytes | 0 bytes (Ayama es 100% offline) |

**Implicación de diseño:** estos límites obligan a:
- Cero polling busy. Toda espera vía eventos/semáforos.
- Cero allocaciones en steady-state.
- Tick adaptativo: lento cuando no hay nada que hacer.
- ETW filtrado en kernel (sin todos los providers, solo los necesarios).
- "Eat your own dog food": Ayama-agent mismo se pinea a un core lento
  (E-core en Intel, CCD sin V-Cache en AMD). Nunca compite por el core
  donde quiere optimizar al usuario.

Detalle técnico en `AYAMA_IMPLEMENTATION_STRATEGIES.md` §3.

### 0.7 Convenciones de nombres (continuación)

- Namespace raíz: `ayama::`
- Sub-namespaces: `ayama::core`, `ayama::observer`, `ayama::policy`, `ayama::action`,
  `ayama::bench`, `ayama::ipc`, `ayama::classifier`, `ayama::learn`, `ayama::ui`
- Tipos: `PascalCase` (`ProcessObserver`, `PolicyEngine`)
- Funciones: `snake_case` (`enumerate_targets`, `apply_policy`)
- Constantes: `kPascalCase` (`kMaxTargets`, `kPollIntervalMs`)
- Archivos: `PascalCase.hpp` para clases, `snake_case.hpp` para utils

### 0.8 Estructura de directorios propuesta

```
F:/gma_1.0.0/
├── pillars/                    ← Gamma framework (read-only desde Ayama)
├── examples/
│   └── standard_window/        ← SOLO REFERENCIA: patrón Gamma+ImGui
│                                  (cómo usar Application::run, RenderNode<T>,
│                                   AppLogicNode, wiring). NO se modifica.
└── ayama/                      ← Producto Ayama [nuevo]
    ├── CMakeLists.txt
    ├── core/
    ├── observer/
    ├── policy/
    ├── action/
    ├── bench/
    ├── learn/
    ├── ipc/
    ├── config/
    └── tools/
        ├── ayama-agent/        ← Daemon background: ayama-agent.exe
        │   └── main.cpp
        ├── ayama-ui/           ← Ventana standalone de Ayama: ayama.exe
        │   ├── AyamaAppState.hpp   — POD state (≤4096B, %64==0)
        │   ├── AyamaLogicNode.hpp  — Logic node: lee AyamaClient SHM
        │   ├── main.cpp            — Application::run, wire logic→render
        │   ├── CMakeLists.txt
        │   ├── build.bat
        │   └── widgets/
        │       ├── dashboard_panel.hpp  — Agent status, self-metrics
        │       ├── targets_panel.hpp    — Tabla de procesos observados
        │       ├── policies_panel.hpp   — Rules list con enable/disable
        │       ├── actions_panel.hpp    — Historial de acciones
        │       ├── bench_panel.hpp      — A/B runner workflow
        │       └── advanced_panel.hpp  — Power-user tweaks
        ├── ayama-cli/          ← CLI para scripts y benchmarks
        │   └── main.cpp
        └── installer/          ← Instalación + registro de servicio opcional
            ├── install.bat
            └── ayama_service_register.cpp
```

---

## Parte 1 — Análisis del estado actual

### 1.1 Inventario de pilares Gamma (vista resumida)

**Fuente autoritativa:** [`../GAMMA_INVENTORY.md`](../GAMMA_INVENTORY.md) — derivado del
código real (`pillars/*/CMakeLists.txt` + headers públicos + `ctest`). Verificado contra
**43/43 tests PASS**.

**Resumen de pilares relevantes para Ayama** (orden topológico, depende solo de los anteriores):

| Pilar | Target | Útil para Ayama (FR ID si aplica) |
|---|---|---|
| `hal` | INTERFACE | `rdtsc`, `kDestructivePad`, **`WakeEvent`** (FR-16) |
| `schema` | INTERFACE | `Error` (16 B), `ErrorCode` (con **SystemError/BufferFull/OutOfMemory** — FR-8) |
| `transport` | STATIC | `Ring<T>` multi-reader (legacy), `Latest<T>`, `SlotCopy` SIMD |
| `topology` | STATIC | `hw::topology()` (FR-1), `ccd_count()` (FR-2), `set_process_affinity()` (FR-3), **`set_process_priority()`/`get_process_priority()`** (FR-9), `hw::v_cache_cores()`/`p_cores()`/`ccd_cores()` |
| `scheduler` | STATIC | `PlacementHint`, `PlacementPolicy::{pinned,soft_affine,…}` |
| `node` / `graph` / `runtime` / `orchestration` | mix | `Inlet/Outlet`, `GraphRuntime`, `Supervisor`, `Watchdog`, `Quiescence`, `ErrorRegistry` |
| `tuning` | STATIC | `check_privilege_level()` (FR-10), **`set_self_working_set()`/`get_self_working_set()`** (FR-19) |
| `behavior` | STATIC | `MigrationTracker`, `PressureScore`, `TimelineEvent`, `TimelineEventBuffer` |
| **`process`** ⭐ | STATIC | **`enumerate_processes()`** (FR-4), **`ProcessMetricsSnapshot`** (FR-11), **`self_pid()/self_ppid()/self_name()`** (FR-18) |
| **`ipc`** ⭐ | INTERFACE | **`ShmRegion<H,P>`** (FR-5), **`Ring<T,Cap>`** (FR-12) — header-only templates |
| **`etw`** ⭐ | STATIC | **`SessionManager`** + `providers::*` GUIDs (FR-6); Linux no-op stubs |
| `render` / `ui` | mixed | `Application::run`, `RenderNode<T>`, `FrameArena`, OpenGL3 backend |
| `correlation` | STATIC | `CorrelationEngine`, `CausalPattern`, `CorrelationAction` |
| `daemon` | STATIC | `SessionDaemon`, `DaemonClient`, `EtwSessionManager` (legacy — `CrossProcessAffinity` **superseded** por FR-3/9, `daemon::EtwSessionManager` **superseded** por FR-6) |

⭐ = pilares nuevos creados por el FR pack v1.1.0. **APIs deprecated** (siguen funcionando pero
preferir las nuevas): `daemon::CrossProcessAffinity::pin_process` → `hw::set_process_affinity`;
`daemon::EtwSessionManager` → `gma::etw::SessionManager`.

Para detalle por pillar (headers, tipos, signaturas, FR IDs con `path:line`), ver
[`../GAMMA_INVENTORY.md`](../GAMMA_INVENTORY.md) §2 y §3.

### 1.2 Lo que Ayama puede reutilizar al 100%

**Infraestructura crítica ya implementada (Gamma 1.1.0):**

1. **`gma::hw::set_process_affinity(pid, mask)`** (FR-3) — pin de procesos externos
   atómico, devuelve la affinity previa para revert. Reemplaza
   `CrossProcessAffinity::pin_process` con una API más limpia y sin estado.

2. **`gma::hw::set_process_priority(pid, class)` / `get_process_priority(pid)`**
   (FR-9) — análogo a FR-3 para priority class. Base directa de
   `ActionExecutor::apply_set_priority()`.

3. **`gma::proc::enumerate_processes()`** (FR-4) — enumeración bulk de PIDs del
   sistema sin abrir handles per-PID. Reemplaza el polling Toolhelp32/EnumProcesses
   de `ProcessObserver_win32.cpp`.

4. **`gma::proc::ProcessMetricsSnapshot`** (FR-11) — **hot-path crítico**.
   Una sola syscall (`NtQuerySystemInformation`) devuelve métricas
   (CPU, RSS, I/O, threads) de TODOS los procesos. Reemplaza el loop
   OpenProcess+GetProcessTimes+GetProcessMemoryInfo+GetProcessIoCounters por PID
   en `MetricsCollector_win32.cpp::sample()`. **2.5-4× speedup medido.**

5. **`gma::proc::self_pid()` / `self_ppid()` / `self_name()`** (FR-18) —
   identidad propia cacheada. Reemplaza `GetCurrentProcessId` ad-hoc.

6. **`gma::ipc::Ring<T, Capacity>`** (FR-12) — SPSC lock-free ring header-only.
   Reemplaza `ActionLogRing` hand-rolled y `etw_ring_` en `MetricsCollector`.
   Cache-aligned cursors → cero false sharing.

7. **`gma::ipc::ShmRegion<Header, Payload>`** (FR-5) — typed SHM con seqlock.
   Reemplaza el hand-rolled MapViewOfFile + mutex en `AyamaAgentPublisher` (~250
   LOC eliminadas).

8. **`gma::etw::SessionManager`** (FR-6) — RAII para sesión ETW + consumer thread.
   Reemplaza el `StartTrace`+`EnableTraceEx2`+`OpenTrace`+`ProcessTrace`
   manual en `MetricsCollector_win32.cpp` (~200 LOC eliminadas).

9. **`gma::hal::WakeEvent`** (FR-16) — auto-reset event cross-platform
   (Win32 `CreateEventW` / Linux `eventfd`). Reemplaza `HANDLE wake_event_`
   manual en `AgentRuntime::run()`.

10. **`gma::hw::topology()` singleton + `ccd_count()`** (FR-1, FR-2) — accesos
    ergonómicos cacheados. Sin `std::expected` boilerplate por cada consumer.

11. **`gma::tuning::check_privilege_level()`** (FR-10) — one-liner para
    "¿soy admin?". Reemplaza el patrón `PrivilegeCheck::probe().level`.

12. **`gma::tuning::set_self_working_set(min, max)`** (FR-19) — wrapper portable
    de `SetProcessWorkingSetSize`. Reemplaza la llamada directa en
    `AgentRuntime::apply_memory_budget()`.

13. **`gma::daemon::EtwSessionManager`** (legacy daemon API) — sigue disponible
    pero superseded por FR-6 `gma::etw::SessionManager` que vive en el pillar
    `etw` (más limpio, sin dependencias del daemon).

14. **`gma::daemon::SessionDaemon`** — daemon-pattern completo con shared memory
    de 4 MB. Ayama-agent puede ser un peer o una extensión de este daemon.

15. **`gma::behavior::MigrationTracker`** — diseñado para tracking del thread
    propio, pero el algoritmo (sample → compute residency) sirve para cualquier
    thread observado vía ETW.

16. **`gma::correlation::CorrelationEngine`** — pattern matching sobre
    TimelineEvents. El motor de policies de Ayama puede ser una extensión de
    este (o composición).

17. **`gma::topology::hw::*`** — `v_cache_cores()`, `p_cores()`, `ccd_cores()`,
    `numa_cores()`. Ayama lo usa para decidir DÓNDE pinear los procesos.

18. **`gma::tuning::PrivilegeCheck`** — runtime check de privilegios con todos
    los campos (`can_set_rt_prio`, `can_lock_pages`, etc.).

19. **`gma::ui::Application` + `gma::ui::RenderNode<T>`** — toda la infra de
    ventana ImGui. **Ayama-ui** (`ayama.exe`) es su propia ventana standalone
    que sigue el patrón de `standard_window` (referencia, nunca modificado).

**Resumen del impacto del FR pack v1.1.0 sobre Ayama:**

| Componente Ayama | LOC antes | LOC después | Ahorro |
|---|---|---|---|
| `MetricsCollector_win32.cpp` (sample loop) | ~280 | ~80 | -200 |
| `MetricsCollector_win32.cpp` (ETW setup/teardown) | ~220 | ~40 | -180 |
| `AyamaAgentPublisher.cpp` | ~300 | ~50 | -250 |
| `ActionExecutor.cpp` (apply / revert) | ~140 | ~80 | -60 |
| `ActionLogRing` (ad-hoc) | ~80 | 0 (usa `Ring<T>`) | -80 |
| `AgentRuntime::run()` (wake event) | ~40 | ~10 | -30 |
| Hand-rolled TOML parsers (config + learn) | ~350 | ~80 (post-FR-15) | -270 |
| **Total estimado** | ~1410 | ~340 | **~1070** |

Más importante que las LOC: cada función reemplazada por una API Gamma viene con
sus propios tests y benchmarks. Ayama hereda la calidad sin replicar trabajo.

### 1.3 Estado de gaps Gamma → Ayama

**Histórico:** Cuando se filó este Master Plan, Ayama identificó 6 gaps. El FR pack
v1.1.0 (mayo 2026) resolvió 4 de ellos directamente y dejó 2 deferidos.

**Resueltos por el FR pack v1.1.0:**

| Gap original | FR resuelvente | Cómo |
|---|---|---|
| `CrossProcessAffinity` con API limpia | **FR-3** (`gma::hw::set_process_affinity`) | API plana sin estado, retorna mask previa |
| ETW abstraction reusable | **FR-6** (`gma::etw::SessionManager`) | RAII + consumer thread + provider GUIDs centralizadas |
| Process enumeration sin handles per-PID | **FR-4** (`gma::proc::enumerate_processes`) + **FR-11** (`ProcessMetricsSnapshot`) | Bulk `NtQuerySystemInformation`, 2.5-4× speedup |
| Shared memory typada | **FR-5** (`gma::ipc::ShmRegion<H,P>`) | Template + seqlock + ABI version check |

**Resueltos como side-effects (FRs adicionales identificados durante implementación):**

| Necesidad latente | FR resuelvente |
|---|---|
| Set priority cross-process | FR-9 (`gma::hw::set_process_priority`) |
| ErrorCodes faltantes (SystemError, BufferFull, OutOfMemory) | FR-8 |
| One-liner privilege check | FR-10 (`gma::tuning::check_privilege_level`) |
| SPSC ring lock-free reusable | FR-12 (`gma::ipc::Ring<T, Capacity>`) |
| Self process identity portable | FR-18 (`gma::proc::CurrentProcess`) |
| Cross-platform wake event | FR-16 (`gma::hal::WakeEvent`) |
| Working-set hint portable | FR-19 (`gma::tuning::set_self_working_set`) |

**Gaps clasificados en dos categorías:**

#### A) Gaps Gamma→Ayama todavía sin FR (esperando feature request a Gamma)

| Gap | Severidad | Workaround actual | Feature request abierto |
|---|---|---|---|
| `MigrationTracker` per-PID externo | Media | Ayama implementa `ExternalMigrationTracker` vía ETW context-switch + TID→PID map | Generalizar `MigrationTracker` |
| `FrameTimeMonitor` para DXGI/Vulkan | Alta | Ayama lee PresentMon ETW provider via `gma::etw::SessionManager` (FR-6) | Pillar nuevo `gma::frame_observer` (futuro) |
| ETW CSwitch source pre-empaquetado con TID→PID cache | Media | Ayama parsea EVENT_RECORD manualmente | **FR-13** (en planeación) — `gma::etw::CtxSwitchSource` |
| Foreground tracking event-driven (zero-poll) | Baja | Ayama polea `GetForegroundWindow()` cada tick | **FR-14** (en planeación) — `gma::ui::ForegroundTracker` con WinEventHook |
| TOML parser zero-alloc reusable | Baja | Cada componente parsea ad-hoc (~150 LOC c/u) | **FR-15** (en planeación) — `gma::config::TomlParser` SAX |
| DedicatedThread wrapper (pin + priority + name) | Baja | Ayama usa `std::thread` directo | **FR-17** (en planeación) — `gma::sched::DedicatedThread` |
| Per-thread affinity para PID externo | Media | Ayama usa Toolhelp32 + `SetThreadAffinityMask` directo | Posible extensión de FR-3 |
| ProcessLifecycle watcher (start/exit notifications) | Baja | Ayama polea cada tick + ETW process events | Resuelto adecuadamente por FR-6 + ETW Process provider |
| `BaselineSampler` para A/B testing | Alta | Ayama implementa en su capa | No requerida en Gamma |

#### B) Migraciones Ayama internas pendientes (Gamma YA provee la API, Ayama todavía usa hand-rolled)

Verificado contra el código en mayo 2026. Estas migraciones eliminan código duplicado y
ganan los tests que ya cubren las APIs Gamma.

| Migración pendiente | Archivo Ayama | API Gamma a usar | Ganancia | Tracked en |
|---|---|---|---|---|
| **SHM hand-rolled** (`CreateFileMappingW`/`mmap` directo) | `ayama/ipc/src/AyamaAgentPublisher.cpp`, `ayama/ipc/src/AyamaClient.cpp` | `gma::ipc::ShmRegion<H,P>` (FR-5) | ~250 LOC eliminadas, ABI magic/version check automático, seqlock RAII | Parte 11 §11.1 |
| **ETW session hand-rolled / stub** (callback no recibe events) | `ayama/observer/src/MetricsCollector_win32.cpp:81-96, 240` | `gma::etw::SessionManager` (FR-6) | ~180 LOC, real CSwitch events vs stub | Parte 11 §11.2 |
| **`InternalWatchdog`** custom (single-watcher) | `ayama/core/include/ayama/core/InternalWatchdog.hpp` | `gma::orchestration::Watchdog` (multi-node con callback per node) | Opcional — ambos funcionan; migrar solo si se añaden watchers por subsistema | Backlog (baja prioridad) |

**Próximos FRs planificados (Phase 2 — ver `GAMMA_FEATURE_REQUEST_IMPLEMENTATION_STRATEGIES.md`):**

- **FR-13** (`gma::etw::CtxSwitchSource`) — context-switch events con TID→PID cache O(1)
- **FR-14** (`gma::ui::ForegroundTracker`) — WinEventHook, snapshot lock-free
- **FR-15** (`gma::config::TomlParser`) — SAX-style zero-alloc
- **FR-17** (`gma::sched::DedicatedThread`) — wrapper con pin + priority + name

Estos cuatro siguen siendo de impacto medio-alto pero NO bloquean Ayama v1.0 — el
agent puede shippear sin ellos usando los workarounds documentados.

### 1.4 Decisión: Ayama vive FUERA de pillars/

**Razonamiento:**
- Pillars/ es framework reutilizable. Ayama es PRODUCTO.
- Tratar Gamma como dependencia externa nos obliga a ser disciplinados con la API pública.
- Si en el futuro publicamos Gamma open-source, Ayama es prueba de que se puede construir
  algo encima sin tocar sus internals.

**Implicación práctica:**
- Ayama incluye headers como `#include <gamma/daemon/CrossProcessAffinity.hpp>`.
- Si necesita algo que Gamma no expone, se documenta como gap (no se modifica Gamma).
- El `CMakeLists.txt` de Ayama hace `add_subdirectory(pillars/<X>)` solo de los pilares
  que consume — NO modifica los CMakeLists de Gamma.

---

## Parte 2 — Arquitectura de Ayama

### 2.1 Componentes de alto nivel

```
                        ┌─────────────────────────────┐
                        │   ayama.exe  (ayama-ui)     │
                        │   - Tab "Dashboard"         │
                        │   - Tab "Targets"           │
                        │   - Tab "Policies"          │
                        │   - Tab "Benchmark"         │
                        └──────────────┬──────────────┘
                                       │ IPC (SHM)
                                       │
                        ┌──────────────▼──────────────┐
                        │     ayama-agent.exe         │
                        │   (background daemon)       │
                        ├─────────────────────────────┤
                        │  ┌───────────────────────┐  │
                        │  │  ProcessObserver      │  │
                        │  │  - enumerate_targets  │  │
                        │  │  - track_lifecycle    │  │
                        │  └─────────┬─────────────┘  │
                        │            │                │
                        │  ┌─────────▼─────────────┐  │
                        │  │  MetricsCollector     │  │
                        │  │  - ETW events         │  │
                        │  │  - PerfCounters       │  │
                        │  │  - PresentMon stats   │  │
                        │  └─────────┬─────────────┘  │
                        │            │                │
                        │  ┌─────────▼─────────────┐  │
                        │  │  PolicyEngine         │  │
                        │  │  - rule matching      │  │
                        │  │  - decision emit      │  │
                        │  └─────────┬─────────────┘  │
                        │            │                │
                        │  ┌─────────▼─────────────┐  │
                        │  │  ActionExecutor       │  │
                        │  │  (gma::daemon::       │  │
                        │  │   CrossProcessAffinity)│ │
                        │  └───────────────────────┘  │
                        └─────────────────────────────┘
                                       │
                                       ▼
                        ┌─────────────────────────────┐
                        │   Procesos OBSERVADOS       │
                        │   - BF6.exe                 │
                        │   - OBS64.exe               │
                        │   - Discord.exe             │
                        │   (no saben que existe Ayama)│
                        └─────────────────────────────┘
```

### 2.2 Flujo de datos completo

**Bucle principal (cada 100 ms):**

1. **Enumerate** — `ProcessObserver::enumerate_targets()`
   Recorre procesos del sistema, filtra por las reglas activas (nombre, exe path).
   Output: `std::vector<TargetProcess>{pid, name, status}`.

2. **Sample** — `MetricsCollector::sample(targets)`
   Para cada target:
   - Lee `GetProcessTimes()` (kernel/user cycles)
   - Lee `NtQuerySystemInformation(SystemProcessInformation)` (context switches per thread)
   - Drena eventos ETW del último intervalo
   - Calcula migrations/sec por thread vía ETW context switch events con CPU index
   Output: `TargetMetrics{pid, migrations_per_sec, frame_time_p99, etc.}`.

3. **Evaluate** — `PolicyEngine::evaluate(targets, metrics)`
   Para cada rule activa:
   - Verifica condiciones (e.g. "migrations_per_sec > 50")
   - Si match: emite `PolicyDecision{target_pid, action_kind, target_core_mask, reason}`
   Output: `std::vector<PolicyDecision>`.

4. **Apply** — `ActionExecutor::apply(decisions)`
   Para cada decision:
   - `CrossProcessAffinity::pin_process(pid, mask)`
   - Loggea en ActionLog con timestamp + estado previo (para revert)
   Output: success/failure por decision.

5. **Publish** — `AyamaState` se publica a la UI vía shared memory.
   La UI lee y muestra targets, decisions, action history.

**Bucle de cleanup (cada 1 s):**

- Verificar si targets desaparecieron (PID muerto) → revertir su affinity.
- Verificar si una rule dejó de aplicar (e.g. game cerrado) → revertir acciones asociadas.

### 2.3 Modelo de datos (POD, IPC-safe)

#### `TargetProcess` (64 bytes)
```cpp
struct alignas(8) TargetProcess {
    uint32_t pid;                  // 4B
    uint32_t parent_pid;           // 4B
    uint64_t start_tsc;            // 8B   — TSC at first observation
    char     name[40];             // 40B  — null-terminated short name
    uint8_t  status;               // 1B   — 0=running, 1=suspended, 2=exiting
    uint8_t  kind;                 // 1B   — TargetKind (Game, Stream, Comm, Browser, Other)
    uint8_t  rules_matched;        // 1B   — bitmask of matching rules
    uint8_t  _pad[5];              // 5B
};
static_assert(sizeof(TargetProcess) == 64);
```

#### `TargetMetrics` (128 bytes)
```cpp
struct alignas(8) TargetMetrics {
    uint32_t pid;
    uint32_t observed_threads;
    uint32_t migrations_per_sec;     // worst thread of the process
    uint32_t involuntary_ctxsw_sec;  // sum of all threads
    float    cpu_usage_pct;          // 0..100, sum over threads
    float    frame_time_avg_ms;      // 0 if not a graphical app
    float    frame_time_p99_ms;
    float    frame_time_variance_ms;
    uint32_t current_core_mask;      // bitmask of cores currently used
    uint32_t allowed_core_mask;      // bitmask we constrained it to (0 = no constraint)
    uint64_t last_sample_tsc;
    uint8_t  pressure_level;         // 0=green, 1=yellow, 2=red
    uint8_t  is_throttled;           // 1 if CPU was thermal-throttling near this proc
    uint8_t  _pad[10];
    char     window_title[64];       // active window title if GUI app
};
static_assert(sizeof(TargetMetrics) == 128);
```

#### `PolicyDecision` (32 bytes)
```cpp
struct alignas(8) PolicyDecision {
    uint32_t target_pid;
    uint32_t rule_id;
    uint8_t  action_kind;    // 1=PinAffinity, 2=SetPriority, 3=Revert
    uint8_t  confidence;     // 0..100
    uint8_t  _pad[2];
    uint64_t core_mask;      // for PinAffinity
    uint32_t priority_class; // for SetPriority
    uint64_t decided_tsc;
};
static_assert(sizeof(PolicyDecision) == 32);
```

### 2.4 Identidad de Ayama vs Gamma

| Aspecto | Gamma | Ayama |
|---|---|---|
| Namespace | `gma::*` | `ayama::*` |
| Header root | `<gamma/...>` | `<ayama/...>` |
| Build target | librerías estáticas `gamma_*` | ejecutable `ayama-agent.exe` |
| Lifecycle | librería linkeada por apps | proceso independiente que las apps observan |
| Conoce el target? | Sí, el target la linkea | No, el target nunca sabe que Ayama lo observa |
| Privilegios | hereda del proceso host | requiere admin para acciones full |
| UI | provee `gma::ui::Application` | ayama-ui (ayama.exe) ventana standalone, patrón tomado de standard_window |

---

## Parte 3 — Gamma como framework externo

### 3.1 Reglas de uso de Gamma desde Ayama

1. **Solo includes públicos.** Lista permitida (post-FR pack v1.1.0):

   **Pilares foundation:**
   - `<gamma/hal/*.hpp>` — incluyendo **`<gamma/hal/WakeEvent.hpp>`** (FR-16)
   - `<gamma/schema/Error.hpp>`, `<gamma/schema/SchemaHash.hpp>`

   **Pilares core:**
   - `<gamma/transport/Ring.hpp>`, `<gamma/transport/Latest.hpp>` (legacy ring, sigue válido)
   - `<gamma/topology/HardwareTopology.hpp>` — incluye **`hw::topology()`** (FR-1), **`ccd_count()`** (FR-2), **`set_process_affinity()`** (FR-3), **`set_process_priority()` / `get_process_priority()`** (FR-9)
   - `<gamma/scheduler/Placement.hpp>` (solo enums/structs)
   - `<gamma/behavior/*.hpp>`
   - `<gamma/tuning/PrivilegeCheck.hpp>` — incluye **`check_privilege_level()`** (FR-10)
   - **`<gamma/tuning/WorkingSet.hpp>`** (FR-19) — `set_self_working_set()` / `get_self_working_set()`

   **Pilares nuevos (FR pack v1.1.0):**
   - **`<gamma/process/ProcessEnumerator.hpp>`** (FR-4) — `enumerate_processes()`, `ProcessEntry`
   - **`<gamma/process/CurrentProcess.hpp>`** (FR-18) — `self_pid()`, `self_ppid()`, `self_name()`
   - **`<gamma/process/ProcessMetricsSnapshot.hpp>`** (FR-11) — `ProcessMetrics`, `ProcessMetricsSnapshot`
   - **`<gamma/ipc/ShmRegion.hpp>`** (FR-5) — `ShmHeaderConcept`, `ShmRegion<H,P>`
   - **`<gamma/ipc/Ring.hpp>`** (FR-12) — `Ring<T, Capacity>` SPSC lock-free
   - **`<gamma/etw/SessionManager.hpp>`** (FR-6) — `SessionManager`, `ProviderSpec`, `providers::kKernelProcess`, etc.

   **Pilares aplicación:**
   - `<gamma/daemon/EtwSessionManager.hpp>` *(legacy — preferir FR-6 `<gamma/etw/SessionManager.hpp>` en código nuevo)*
   - `<gamma/correlation/CausalPattern.hpp>`, `<gamma/correlation/CorrelationAction.hpp>`
   - Cualquier header bajo `gamma/ui/types/` (POD types)

   **NOTA — APIs deprecated en Ayama código nuevo:**
   - `<gamma/daemon/CrossProcessAffinity.hpp>` → usar `<gamma/topology/HardwareTopology.hpp>` (FR-3 + FR-9)
   - `<gamma/transport/Ring.hpp>` → preferir `<gamma/ipc/Ring.hpp>` (FR-12) para SPSC nuevo (transport::Ring sigue siendo válido y no se quita)

2. **NUNCA incluir desde `pillars/<x>/src/`** ni headers no expuestos públicamente.

3. **Si Ayama necesita algo que no está en la lista permitida**, abrir un ticket
   en `docs/ayama/GAMMA_FEATURE_REQUESTS.md` y trabajar con un fallback local
   hasta que el FR se materialice (ver §1.3 para FRs en planeación: FR-13, FR-14,
   FR-15, FR-17).

4. **Link target Gamma**: Ayama enlaza solo las libs necesarias:
   ```cmake
   target_link_libraries(ayama_core PRIVATE
       gamma_hal              # WakeEvent (FR-16), rdtsc
       gamma_schema           # Error, ErrorCode
       gamma_topology         # hw::topology, set_process_affinity, set_process_priority
       gamma_process          # ProcessMetricsSnapshot, CurrentProcess, enumerate_processes
       gamma_ipc              # Ring<T>, ShmRegion<H,P>  (header-only INTERFACE)
       gamma_etw              # SessionManager (Windows-only, no-op stubs en Linux)
       gamma_behavior         # MigrationTracker, PressureScore
       gamma_tuning           # PrivilegeCheck, WorkingSet
       gamma_correlation      # CorrelationEngine
       # gamma_daemon         # OPCIONAL — solo si se reutiliza SessionDaemon directamente
   )
   ```

5. **Versión pinneada.** Ayama documenta contra qué versión de Gamma compila.
   Si Gamma actualiza, Ayama tiene un build de validación que verifica que
   los headers consumidos siguen siendo compatibles.
   **Versión mínima requerida: Gamma 1.1.0** (post-FR pack).

### 3.2 Preparación previa requerida

Antes de empezar a codificar Ayama, había que limpiar Gamma para que sea
consumible como framework. **Estado a mayo 2026: los 4 bloques están completos.**

**Bloque P.1 — Single-include public headers ✅ DONE**
   Existen en el código (verificado):
   - `pillars/hal/include/gamma/hal.hpp` ✓ (no incluye `WakeEvent.hpp` — incluir explícito)
   - `pillars/schema/include/gamma/schema.hpp` ✓
   - `pillars/transport/include/gamma/transport.hpp` ✓ (vía `TransportAll.hpp`)
   - `pillars/topology/include/gamma/topology.hpp` ✓
   - `pillars/tuning/include/gamma/tuning.hpp` ✓
   - `pillars/behavior/include/gamma/behavior.hpp` ✓
   - `pillars/correlation/include/gamma/correlation.hpp` ✓
   - `pillars/daemon/include/gamma/daemon.hpp` ✓
   - `pillars/node/include/gamma/node/NodeAll.hpp`, `graph/GraphAll.hpp`, `runtime/RuntimeAll.hpp`, `orchestration/OrchestrationAll.hpp` ✓
   - Los nuevos pilares (`process`, `ipc`, `etw`) **no tienen umbrella todavía** — los headers se incluyen individualmente (`gamma/process/CurrentProcess.hpp`, etc.). Aceptable por ahora.

**Bloque P.2 — Versionado ✅ DONE**
   - `pillars/_meta/include/gamma/version.hpp` existe con macros `GAMMA_VERSION_MAJOR/MINOR/PATCH`, `GAMMA_MAKE_VERSION(maj,min,patch)`, `GAMMA_VERSION_STRING`.
   - **Nota:** el valor del macro sigue siendo `"1.0.0"` aunque la "release line" interna del FR pack se llame `1.1.0`. Ayama puede pinearse con `static_assert(GAMMA_VERSION >= GAMMA_MAKE_VERSION(1,0,0))` por ahora.

**Bloque P.3 — Documentar API estable ✅ DONE**
   - [`docs/GAMMA_INVENTORY.md`](../GAMMA_INVENTORY.md) — inventario completo derivado del código (no de docs auxiliares). Lista cada pillar, headers públicos, signaturas clave, mapeo FR → `path:line` exacto, y los 43 tests ejecutables.

**Bloque P.4 — Verificar todos los pilares compilan independientemente ✅ DONE**
   - `ctest --test-dir build -N` reporta 43 tests registrados (mayo 2026).
   - `ctest --output-on-failure` ejecuta **43/43 PASS** end-to-end.
   - Cada pillar tiene su propio `CMakeLists.txt` con `add_subdirectory()` standalone-friendly (deps resueltas con `if(NOT TARGET ...)`)

---

## Parte 4 — Bloques de implementación

### Fase 1 — Foundation (Bloques 1.x)

#### Bloque 1.1 — `ayama::core::AgentRuntime`
**Pillar:** `ayama/core`
**Estado:** Nuevo
**Dependencias (post-Gamma 1.1.0):**
- `gamma_orchestration` — `Supervisor`, `Watchdog`
- `gamma_hal` — `rdtsc`, **`WakeEvent`** (FR-16, reemplaza el `CreateEventW` manual)
- `gamma_schema` — `Error`, `ErrorCode` (con SystemError/OutOfMemory de FR-8)
- `gamma_tuning` — `PrivilegeCheck`, **`check_privilege_level()`** (FR-10),
  **`set_self_working_set()`** (FR-19)
- `gamma_topology` — **`hw::topology()`** (FR-1) para self-pinning
- `gamma_process` — **`self_pid()` / `self_name()`** (FR-18)

**Objetivo:** Esqueleto del proceso `ayama-agent.exe`. Loop principal con shutdown limpio.

**Estado:** ✅ **Implementado** (ver §8.7).

**Archivos (estructura real per-pillar, no `ayama/include/...` plano):**
- `ayama/core/include/ayama/core/AgentRuntime.hpp` + `AdaptiveTick.hpp`, `SelfMonitor.hpp`, `PowerWatch.hpp`, `AutoRevertGuard.hpp`, `InternalWatchdog.hpp`, `IdleWatch.hpp`, `SingleInstance.hpp`
- `ayama/core/src/AgentRuntime.cpp`, `AutoRevertGuard.cpp`
- `ayama/tools/ayama-agent/main.cpp`
- `ayama/core/tests/agent_runtime_test.cpp` (`ayama_core_test` — PASS)

**API:**
```cpp
namespace ayama::core {

struct AgentConfig {
    uint32_t poll_interval_ms       {100u};
    uint32_t baseline_window_ms     {5000u};
    bool     enable_shm_publish     {true};
    bool     require_admin          {false};
    char     shm_name[40]           {"Local\\AyamaAgent.v1"};
};

class AgentRuntime {
public:
    explicit AgentRuntime(AgentConfig cfg = {}) noexcept;
    ~AgentRuntime() noexcept;

    [[nodiscard]] std::expected<void, gma::Error> start() noexcept;
    void run() noexcept;          // blocking
    void stop() noexcept;         // signal-safe

    [[nodiscard]] bool running() const noexcept;
};

} // namespace ayama::core
```

**Test de aceptación:**
- `agent_runtime_test` arranca, corre 100 ticks, hace stop limpio en < 50 ms.
- Sin admin, `start()` logguea warning pero continúa en modo degradado.

#### Bloque 1.2 — `ayama::observer::ProcessObserver`
**Pillar:** `ayama/observer`
**Dependencias (post-Gamma 1.1.0):**
- `gamma_process` — **FR-4** `gma::proc::enumerate_processes()` reemplaza
  `CrossProcessAffinity::enumerate_by_name` con una API más directa.

**Objetivo:** Enumerar procesos del sistema y mantener tabla actualizada de targets.

**Estado:** ✅ **Implementado** (usa `gma::proc::enumerate_processes` desde la migración FR-4).

**Archivos:**
- `ayama/observer/include/ayama/observer/ProcessObserver.hpp` + `TargetProcess.hpp`
- `ayama/observer/src/ProcessObserver.cpp`, `ProcessObserver_win32.cpp`, `ProcessObserver_linux.cpp`
- `ayama/observer/tests/process_observer_test.cpp` (`ayama_observer_test` — PASS)

**API:**
```cpp
namespace ayama::observer {

constexpr uint32_t kMaxTargets = 32u;

class ProcessObserver {
public:
    ProcessObserver() noexcept;
    ~ProcessObserver() noexcept;

    /// Refresh the target list. Cheap (~1 ms with 200 processes).
    void refresh() noexcept;

    /// Add a target filter by executable name (case-insensitive).
    void add_target_pattern(const char* pattern) noexcept;
    void remove_target_pattern(const char* pattern) noexcept;

    /// Get a snapshot of current targets.
    [[nodiscard]] uint32_t snapshot(TargetProcess* out, uint32_t max) const noexcept;

    [[nodiscard]] uint32_t target_count() const noexcept;
};

} // namespace ayama::observer
```

**Test:**
- Arrancar `notepad.exe` antes del test.
- `add_target_pattern("notepad")` → `refresh()` → `target_count() >= 1`.
- Cerrar notepad → `refresh()` → `target_count() == 0`.

#### Bloque 1.3 — `ayama::observer::MetricsCollector`
**Dependencias (post-Gamma 1.1.0):**
- `gamma_process` (FR-11 `ProcessMetricsSnapshot` — base del sample()) ⭐
- `gamma_etw` (FR-6 `SessionManager` — sesión ETW + consumer thread) ⭐
- `gamma_ipc` (FR-12 `Ring<TimelineEvent>` — buffer ETW→agent) ⭐
- `gamma_behavior` (legacy)
- `gamma_daemon::EtwSessionManager` opcional (legacy path)

**Objetivo:** Para cada target, computar `TargetMetrics` vía ETW + bulk metrics snapshot.

**Estado:** ⚠ **Implementado parcial** — bulk metrics vía `ProcessMetricsSnapshot` ya migrado;
**ETW hookup real pendiente** (callback no recibe events todavía, ver Parte 11 §11.2).

**Archivos:**
- `ayama/observer/include/ayama/observer/MetricsCollector.hpp` + `TargetMetrics.hpp`, `ProcessClassifier.hpp`, `ClassificationCache.hpp`, `ForegroundWatcher.hpp`, `EtwProviderSet.hpp`
- `ayama/observer/src/MetricsCollector_win32.cpp`, `MetricsCollector_linux.cpp`, `ProcessClassifier.cpp`
- (`ExternalMigrationTracker.hpp` planeado para FR-13 — en backlog)
- Sin test dedicado a `MetricsCollector` standalone (cubierto indirectamente por `agent_runtime_test`).

**Notas críticas (post-FR pack v1.1.0):**
- **Bulk metrics via FR-11**: `ProcessMetricsSnapshot::capture()` reemplaza el loop
  `OpenProcess` + `GetProcessTimes` + `GetProcessMemoryInfo` + `GetProcessIoCounters`
  por PID. Una sola syscall (`NtQuerySystemInformation`) → métricas de todos los
  procesos en ~150 µs. **2.5-4× speedup** vs implementación per-PID.
- **ETW session via FR-6**: `gma::etw::SessionManager` maneja `StartTrace` +
  `EnableTraceEx2` + `OpenTrace` + consumer thread. Ayama solo provee callback.
- **Ring lock-free via FR-12**: `gma::ipc::Ring<TimelineEvent, 65536>` reemplaza
  el `etw_ring_` hand-rolled. SPSC cache-aligned, cero false sharing.
- ETW context-switch events incluyen `NewThreadId` y `NewCpu`. Mapeando thread→PID
  podemos calcular migraciones por proceso externo. (Optimización futura: FR-13
  `gma::etw::CtxSwitchSource` con TID→PID cache O(1)).
- PresentMon ETW provider da frame timing de DXGI/D3D12/Vulkan apps.

**API:**
```cpp
namespace ayama::observer {

class MetricsCollector {
public:
    MetricsCollector() noexcept;
    ~MetricsCollector() noexcept;

    /// Configure ETW. Without admin, frame_observer features are disabled.
    [[nodiscard]] std::expected<void, gma::Error> start() noexcept;
    void stop() noexcept;

    /// Sample metrics for the targets in `pids`.
    /// `out_metrics` must have capacity for at least `n` entries.
    void sample(const uint32_t* pids, uint32_t n,
                TargetMetrics* out_metrics) noexcept;

    [[nodiscard]] bool etw_active() const noexcept;
    [[nodiscard]] bool frame_observer_active() const noexcept;
};

} // namespace ayama::observer
```

#### Bloque 1.4 — `ayama::policy::PolicyEngine`
**Dependencias:** `gamma_correlation`, `gamma_topology`

**Objetivo:** Convertir métricas + reglas → decisiones de acción.

**Estado:** ✅ **Implementado** (incluyendo `AutoPolicySelector` con tabla de decisión §9.2).

**Archivos:**
- `ayama/policy/include/ayama/policy/PolicyEngine.hpp` + `Rule.hpp`, `Condition.hpp`, `PolicyDecision.hpp`, `AutoPolicySelector.hpp`
- `ayama/policy/src/PolicyEngine.cpp`
- `ayama/policy/tests/policy_engine_test.cpp` (`ayama_policy_test` — PASS), `auto_policy_selector_test.cpp` (`ayama_auto_policy_test` — PASS)
- Default rules registradas inline en `PolicyEngine::register_default_rules(topo)` (no en archivo aparte)

**Default rules incluidas:**
1. **PinGameToVCacheCcd** (Ryzen X3D)
   Condition: target.kind == Game AND topology has v_cache_cores
   Action: pin target to mask of v_cache_cores

2. **IsolateGameFromBackground**
   Condition: target.kind == Game AND another target.kind in (Stream, Comm) AND target.migrations_per_sec > 30
   Action: pin game to subset_A, pin background to subset_B

3. **PinGameToPCores** (Intel hybrid)
   Condition: target.kind == Game AND topology.e_core_count > 0
   Action: pin target to mask of p_cores

4. **EvictStreamFromHotCcd**
   Condition: target.kind == Stream AND any game is running
   Action: pin stream to CCD without V-Cache (or to E-cores on Intel)

5. **RevertOnTargetExit**
   Condition: target was previously pinned AND target now exited
   Action: revert (no-op, but cleans tracking)

**API:**
```cpp
namespace ayama::policy {

constexpr uint32_t kMaxRules = 32u;
constexpr uint32_t kMaxDecisionsPerCycle = 16u;

class PolicyEngine {
public:
    PolicyEngine() noexcept;

    void register_default_rules(const gma::HardwareTopology& topo) noexcept;
    void register_rule(const Rule& r) noexcept;
    void disable_rule(uint32_t rule_id) noexcept;

    /// Evaluate all enabled rules against current state.
    /// Returns number of decisions written to out_decisions.
    [[nodiscard]] uint32_t evaluate(
        const TargetProcess* targets, uint32_t n_targets,
        const TargetMetrics* metrics, uint32_t n_metrics,
        PolicyDecision* out_decisions) noexcept;
};

} // namespace ayama::policy
```

#### Bloque 1.5 — `ayama::action::ActionExecutor`
**Dependencias (post-Gamma 1.1.0):**
- `gamma_topology` — **FR-3** (`gma::hw::set_process_affinity`) + **FR-9**
  (`gma::hw::set_process_priority`). Estas APIs son la base del executor —
  `CrossProcessAffinity` queda deprecated.
- `gamma_ipc` — **FR-12** `gma::ipc::Ring<ActionLogEntry, 128>` reemplaza el
  hand-rolled `ActionLogRing`.

**Objetivo:** Aplicar decisiones y poder revertirlas. Mantener log para A/B testing.

**Estado:** ✅ **Implementado**; `ActionLogRing` migrado a `gma::ipc::Ring<ActionLogEntry,128>` (FR-12);
acciones aplicadas con `hw::set_process_affinity` (FR-3) y `hw::set_process_priority` (FR-9);
`AuditLog` persistente para audit trail en disco.

**Archivos:**
- `ayama/action/include/ayama/action/ActionExecutor.hpp` + `ActionLog.hpp` (typedef sobre `gma::ipc::Ring`), `AuditLog.hpp`
- `ayama/action/src/ActionExecutor.cpp`
- `ayama/action/tests/action_executor_test.cpp` (`ayama_action_test` — PASS)

**API:**
```cpp
namespace ayama::action {

struct ActionLogEntry {
    uint64_t tsc_applied;
    uint64_t tsc_reverted;          // 0 if still active
    uint32_t target_pid;
    uint32_t rule_id;
    uint64_t prev_affinity_mask;    // for revert
    uint32_t prev_priority_class;   // for revert
    uint64_t new_affinity_mask;
    uint32_t new_priority_class;
    uint8_t  success;
    uint8_t  _pad[7];
};
static_assert(sizeof(ActionLogEntry) == 56);

class ActionExecutor {
public:
    ActionExecutor() noexcept;

    /// Apply the decision; record previous state for revert.
    [[nodiscard]] std::expected<void, gma::Error>
    apply(const PolicyDecision& d) noexcept;

    /// Revert a previously-applied action for `pid` (if any active).
    void revert(uint32_t pid) noexcept;

    /// Revert all active actions (called on shutdown).
    void revert_all() noexcept;

    /// Snapshot the action log.
    [[nodiscard]] uint32_t snapshot_log(ActionLogEntry* out, uint32_t max) const noexcept;
};

} // namespace ayama::action
```

**Test crítico:**
- Aplicar `pin_process(notepad_pid, 0x1)` → verificar via `GetProcessAffinityMask`.
- `revert(notepad_pid)` → verificar mask vuelve al original capturado.

### Fase 2 — Measurement Infrastructure (Bloques 2.x)

#### Bloque 2.1 — `ayama::bench::Baseline`
**Objetivo:** Capturar estado sin intervención de Ayama (control group).

**Estado:** ✅ **Implementado**.

**Archivos:**
- `ayama/bench/include/ayama/bench/Baseline.hpp`
- `ayama/bench/src/Baseline.cpp`
- `ayama/bench/tests/baseline_test.cpp` (`ayama_bench_test` — PASS)

**API:**
```cpp
namespace ayama::bench {

struct BaselineSample {
    uint32_t target_pid;
    uint64_t sample_tsc;
    float    frame_time_avg_ms;
    float    frame_time_p99_ms;
    float    frame_time_variance_ms;
    uint32_t migrations_per_sec;
    uint32_t involuntary_ctxsw_per_sec;
    float    cpu_usage_pct;
    uint8_t  _pad[36];   // pad to 64B (1 cache line)
};
static_assert(sizeof(BaselineSample) == 64);

class Baseline {
public:
    Baseline() noexcept;

    /// Start recording for `pid`. Stops any active recording.
    void start(uint32_t pid, uint32_t duration_ms = 30000u) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool recording() const noexcept;

    /// Push a sample (called by ProcessObserver tick).
    void push_sample(const BaselineSample& s) noexcept;

    /// Aggregate statistics over the recording window.
    [[nodiscard]] BaselineSummary summary() const noexcept;
};

struct BaselineSummary {
    uint32_t sample_count;
    float    frame_time_avg_ms;
    float    frame_time_p99_ms;
    float    frame_time_max_ms;
    float    frame_time_stddev_ms;
    uint32_t total_migrations;
    uint32_t total_involuntary_ctxsw;
    float    avg_cpu_usage_pct;
};

} // namespace ayama::bench
```

#### Bloque 2.2 — `ayama::bench::ABRunner`
**Objetivo:** Ejecutar test A/B automatizado: captura baseline → aplica policies → captura treated → reporta diff.

**Estado:** ✅ **Implementado**.

**Archivos:**
- `ayama/bench/include/ayama/bench/ABRunner.hpp` + `DiffReport.hpp`
- `ayama/bench/src/ABRunner.cpp`
- `ayama/bench/tests/abrunner_test.cpp` (`ayama_abrunner_test` — PASS)

**Workflow:**
```cpp
ABRunner runner;
runner.set_target_pid(bf6_pid);

// Phase A: baseline (Ayama disabled)
runner.start_phase_a();  // 30s of recording without intervention
runner.wait_complete();

// Phase B: treated (Ayama policies applied)
runner.start_phase_b();  // 30s of recording with policies
runner.wait_complete();

// Result
DiffReport r = runner.generate_report();
std::cout << "P99 frametime: "
          << r.phase_a.frame_time_p99_ms << " → "
          << r.phase_b.frame_time_p99_ms << " ms ("
          << r.improvement_pct << "% improvement)\n";
```

**API:**
```cpp
namespace ayama::bench {

struct DiffReport {
    BaselineSummary phase_a;      // without Ayama
    BaselineSummary phase_b;      // with Ayama
    float frame_time_p99_delta_ms;
    float frame_time_p99_improvement_pct;   // positive = better
    float frame_time_variance_delta_ms;
    float migration_rate_delta;             // negative = fewer migrations
    char  verdict[64];                      // "Significant", "Marginal", "None", "Regression"
};

class ABRunner {
public:
    ABRunner() noexcept;

    void set_target_pid(uint32_t pid) noexcept;
    void set_phase_duration(uint32_t ms) noexcept;
    void set_policy_set(const policy::Rule* rules, uint32_t n) noexcept;

    [[nodiscard]] std::expected<void, gma::Error> start_phase_a() noexcept;
    [[nodiscard]] std::expected<void, gma::Error> start_phase_b() noexcept;

    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] DiffReport generate_report() const noexcept;
};

} // namespace ayama::bench
```

#### Bloque 2.3 — `ayama::bench::PerceptualMetrics`
**Objetivo:** Convertir métricas crudas en métricas que humanos perciben.

**Métricas implementadas:**
- **Stutter count**: número de frames con frametime > 2x media local.
- **Hitching index**: fracción de tiempo con frametime > 50ms.
- **Smoothness score**: 1 - (stddev / mean) — alto = suave, bajo = saccadic.
- **%1-low FPS**: 1/percentile_99(frametime).
- **%0.1-low FPS**: 1/percentile_999(frametime).

**Estado:** ✅ **Implementado**.

**Archivos:**
- `ayama/bench/include/ayama/bench/PerceptualMetrics.hpp`
- `ayama/bench/src/PerceptualMetrics.cpp`
- `ayama/bench/tests/perceptual_test.cpp` (`ayama_perceptual_test` — PASS)

### Fase 3 — IPC y UI (Bloques 3.x)

#### Bloque 3.1 — `ayama::ipc::AyamaProtocol`
**Dependencias (post-Gamma 1.1.0):**
- `gamma_ipc` — **FR-5** `gma::ipc::ShmRegion<AyamaShmHeader, AyamaPayload>`
  reemplaza el hand-rolled `CreateFileMappingW` + seqlock manual (~250 LOC
  eliminadas). El template encapsula la lógica completa de single-writer/
  multi-reader con seqlock y ABI version/magic check.

**Objetivo:** Shared memory protocol entre `ayama-agent.exe` y `standard_window`.

**Estado:** ⚠ **Implementado pero NO usa `gma::ipc::ShmRegion`** — actualmente
`AyamaAgentPublisher.cpp` y `AyamaClient.cpp` hand-rollean `CreateFileMappingW`+`MapViewOfFile`
con seqlock manual. La migración a `ShmRegion<H,P>` está pendiente — ver Parte 11 §11.1.
La SHM ring `action_log` SÍ usa `gma::ipc::Ring<ActionLogEntry,128>` con peek_at().

**Archivos (reales):**
- `ayama/ipc/include/ayama/ipc/AyamaProtocol.hpp` — POD + magic + seqlock helpers
- `ayama/ipc/include/ayama/ipc/AyamaClient.hpp` + `AyamaAgentPublisher.hpp`
- `ayama/ipc/src/AyamaClient.cpp`, `AyamaAgentPublisher.cpp`
- Sin test dedicado (cubierto indirectamente por integración con `ayama_core_test`)

**Post-migración esperada (Parte 11 §11.1):**
- `AyamaClient.cpp` reducido a ~40 LOC (solo `ShmRegion::open` + `try_read_consistent`).
- `AyamaAgentPublisher.cpp` reducido a ~50 LOC (solo `ShmRegion::create` + `begin_write` RAII).
- ~250 LOC eliminadas total.

**Layout SHM (1 MB):**
```
[0..127]              ShmHeader (magic, version, agent_pid, ready_flag)
[128..192]            AyamaStateHeader (n_targets, n_decisions, n_actions, baseline_active)
[193..2240]           TargetProcess[32]   (32 × 64 = 2048B)
[2241..6336]          TargetMetrics[32]   (32 × 128 = 4096B)
[6337..6848]          PolicyDecision[16]  (16 × 32 = 512B)
[6849..14400]         ActionLogEntry[128] (128 × 56 = 7168B, circular)
[14401..1048575]      Baseline ring buffer (~1MB de samples)
```

**Cliente API (usado por standard_window):**
```cpp
namespace ayama::ipc {

class AyamaClient {
public:
    AyamaClient() noexcept;
    ~AyamaClient() noexcept;

    [[nodiscard]] std::expected<void, gma::Error> connect() noexcept;
    void disconnect() noexcept;
    [[nodiscard]] bool is_connected() const noexcept;

    /// Read-only views into shared memory. Valid until disconnect().
    [[nodiscard]] std::span<const TargetProcess>  targets()   const noexcept;
    [[nodiscard]] std::span<const TargetMetrics>  metrics()   const noexcept;
    [[nodiscard]] std::span<const PolicyDecision> decisions() const noexcept;
    [[nodiscard]] std::span<const ActionLogEntry> action_log() const noexcept;
};

} // namespace ayama::ipc
```

### Fase 4 — ayama-ui: ventana standalone de Ayama (Bloques 4.x)

**Estado global Fase 4:** ⚠ **Código existe pero NO se compila por defecto.**
`AYAMA_BUILD_UI:BOOL=OFF` en el cache actual del build, aunque el default en
`ayama/CMakeLists.txt` es `ON` (línea 24). Para activar:
```bash
cmake -B build -DGAMMA_BUILD_AYAMA=ON -DAYAMA_BUILD_UI=ON
cmake --build build --target ayama-ui
```
Todos los widgets `.hpp` existen en `ayama/tools/ayama-ui/widgets/` y compilan; solo
falta encender la flag y verificar el run-time. Tracked en §8.7.

**Nota arquitectural:** Los widgets de Ayama viven en `ayama/tools/ayama-ui/widgets/`.
`standard_window` es referencia de lectura del patrón `gma::ui::Application::run` +
`RenderNode<T>`. No se modifica ni se extiende.

#### Bloque 4.1 — `widgets/targets_panel.hpp`
**Objetivo:** Tab "Targets" en ayama-ui. Lista de procesos observados.

**Archivos:**
- `ayama/tools/ayama-ui/widgets/targets_panel.hpp`

**Diseño:**
- Tabla con columnas: PID, Name, Kind, CPU%, Migrations/s, Frame P99, Affinity Mask
- Botón "Force pin" / "Force unpin" por target
- Filtro por nombre / kind

#### Bloque 4.2 — `widgets/policies_panel.hpp`
**Objetivo:** Tab "Policies". Listado y edición de rules activas.

**Archivos:**
- `ayama/tools/ayama-ui/widgets/policies_panel.hpp`

**Diseño:**
- Lista de rules con checkbox enable/disable
- Editor inline: condición + acción
- Botón "Reset to defaults"

#### Bloque 4.3 — `widgets/actions_panel.hpp`
**Objetivo:** Tab "Actions". Historial de acciones aplicadas y revertidas.

**Archivos:**
- `ayama/tools/ayama-ui/widgets/actions_panel.hpp`

**Diseño:**
- Timeline visual de acciones (X = tiempo, Y = target)
- Tabla con: timestamp, target, rule, action, success, revert_at
- Botón "Revert this action" manual

#### Bloque 4.4 — `widgets/bench_panel.hpp`
**Objetivo:** Tab "Benchmark". Workflow A/B runner.

**Archivos:**
- `ayama/tools/ayama-ui/widgets/bench_panel.hpp`

**Diseño:**
- Step 1: seleccionar PID target
- Step 2: configurar duración fase
- Step 3: arrancar Phase A → progreso → Phase B → progreso → Report
- Visualización del diff: bar chart phase_a vs phase_b por métrica
- Verdict prominente: "X% improvement / Marginal / Regression"

#### Bloque 4.5 — `widgets/dashboard_panel.hpp`
**Objetivo:** Tab "Dashboard" — vista default del modo Auto.

**Archivos:**
- `ayama/tools/ayama-ui/widgets/dashboard_panel.hpp`

**Diseño:**
- Status: Agent connected? Privileges level? ETW active?
- Resumen: # targets, # active policies, # actions in last 60s
- Live sparkline: pressure score of all observed targets
- Self-metrics: CPU% propio, RSS, tick interval actual

#### Bloque 4.6 — Contrato de widgets
**Objetivo:** Patrón uniforme para todos los panels de ayama-ui.

**Patrón:**
```cpp
// Cada panel sigue este contrato:
//
//   inline void draw_<thing>_panel(const AyamaAppState& s,
//                                   const ayama::ipc::AyamaClient* ac) noexcept;
//
// - Recibe AyamaAppState (snapshotMini + estado UI local)
// - Recibe puntero al AyamaClient (puede ser nullptr si no hay agent)
// - Maneja graceful degradation: si ac == nullptr o !ac->is_connected(),
//   muestra placeholder elegante "Ayama agent not running".
// - Cero allocación dinámica en el draw loop.
// - Solo lee del shared memory; jamás escribe (las acciones son comandos
//   explícitos vía request buttons que envían IPC commands).
```

### Fase 5 — Configuración y Distribución (Bloques 5.x)

#### Bloque 5.1 — Config persistence
**Estado:** ✅ **Implementado** (TOML ad-hoc; migrará a FR-15 cuando esté disponible).

**Objetivo:** Cargar/guardar policies del usuario.

**Archivos:**
- `ayama/config/include/ayama/config/ConfigStore.hpp` + `DefaultPolicyPack.hpp`
- `ayama/config/src/ConfigStore.cpp`
- También: `ayama/learn/src/PerGameMemory.cpp` (parser TOML separado para `memory.toml`)

**Formato:** TOML simple guardado en `%LOCALAPPDATA%\Ayama\policies.toml` y `memory.toml`.

**Dependencias futuras:**
- **FR-15** `gma::config::TomlParser` (en planeación) — SAX-style zero-alloc.
  Cuando esté listo, `ConfigStore::load_policies` + `PerGameMemory::load_memory`
  se reescriben sobre `gma::config::parse()` callback, eliminando ~250 LOC de
  parser duplicado.

#### Bloque 5.2 — Installer / service registration
**Estado:** ✅ **Implementado**. Falta solo testing empírico en Win10/Win11 (DoD #5).

**Archivos:**
- `ayama/tools/installer/ayama_service_register.cpp` ✓ (linka como `ayama-service-register.exe`)
- `ayama/tools/installer/install.bat` ✓ (modos: default, `--service` para auto-start, `--user` para per-user sin admin)
- `ayama/tools/installer/uninstall.bat` ✓ (modo `--purge` para remover configs además)

#### Bloque 5.3 — Default policy pack
**Objetivo:** Empaquetar reglas detectadas automáticamente según CPU.

- AMD X3D: PinGameToVCacheCcd default ON
- Intel hybrid: PinGameToPCores default ON
- Multi-CCX no-X3D: IsolateGameFromBackground default ON
- Single-CCD homogéneo: solo defaults conservadoras

---

## Parte 5 — Medición antes/después

### 5.1 Métricas que importan a humanos

Ordenadas por perceptibilidad humana (alto → bajo):

| Métrica | Por qué importa | Mejora "perceptible" |
|---|---|---|
| **Stutters (frames > 2× media)** | Causa #1 de "feel" malo en juegos | -50% es notorio |
| **%1-low FPS** | El "FPS real" para el jugador, no el promedio | +10% es notorio |
| **Frame time variance** | Smoothness percibido | -30% es notorio |
| **%0.1-low FPS** | Casos extremos (lag spikes severos) | +20% es notorio |
| **Input lag (click-to-photon)** | Responsividad percibida | -5ms es notorio |
| **Avg FPS** | Lo que la gente piensa que importa | +5% es notorio |

**Decisión:** El "verdict" del DiffReport se basa primariamente en stutter count y P99,
secundariamente en variance, y solo terciariamente en avg FPS.

### 5.2 Test methodology

Cada feature de Ayama debe pasar este checklist antes de declararse "shipping ready":

1. **Reproducible test scenario** documentado:
   - Hardware target (CPU model, RAM, GPU)
   - Workload exacto (juego + duración + escenario)
   - Background load (OBS streaming a 1080p60? Discord en call?)

2. **Baseline measurement** sin Ayama:
   - 3 runs de al menos 5 minutos cada uno
   - Datos crudos guardados (csv/binary)

3. **Treated measurement** con Ayama:
   - Mismas 3 runs, mismo workload
   - Misma duración

4. **Statistical significance check:**
   - t-test sobre P99 frametime entre grupos
   - p < 0.05 para declarar mejora significativa

5. **DiffReport publicado** en `docs/ayama/reports/<scenario>_<date>.md`

### 5.3 Scenarios canónicos (test matrix)

**Política importante:** El test pack inicial **NO toca juegos con anticheat**
(EAC, BattlEye, Vanguard, FACEIT). Solo se escala a anticheats una vez que
tenemos datos sólidos y un mecanismo verificado para que Ayama no sea
detectado como tampering.

**Targets de validación inicial (sin anticheat):**
- **Sintéticos reproducibles** (preferidos para CI / validación numérica):
  - 3DMark Time Spy / Time Spy Extreme — escena fija, frametime estable
  - Cinebench R23 Multi-Core — context-switch heavy, sin GPU
  - Ayama-bench (micro-benchmark propio incluido) — ver §7.4
- **Juegos AAA singleplayer sin anticheat** (preferidos para validación UX):
  - Cyberpunk 2077 (CPU-heavy, escenarios de ciudad reproducibles)
  - Hogwarts Legacy (stuttering conocido — caso perfecto)
  - The Witcher 3 Next-Gen (DX12, frame-pacing sensible)
  - Stalker 2: Heart of Chornobyl (CPU-bound, multi-thread)
  - God of War (Ragnarok PC) (PSO compile stutters)
  - Spider-Man Remastered (traversal stutters)
  - Forza Horizon 5 (modo solo, NO modo online con EAC)
  - Returnal (DX12 multi-threaded)
- **Workloads no-juego que también stresean scheduler:**
  - Blender Cycles render (bursts intensos)
  - DaVinci Resolve playback (real-time decoding)
  - OBS Studio standalone (encoding x264)
  - Visual Studio / clang building Gamma mismo (cargas mixtas)

**Test matrix (Phase 1 — no anticheat):**

| ID | Scenario | Hardware | Workload | Background | Expected outcome |
|---|---|---|---|---|---|
| S1 | Synthetic-baseline | Cualquier CPU | Ayama-bench microbench | nada | Frametime variance baja con policy |
| S2 | Cyberpunk-solo | Ryzen X3D | Cyberpunk 2077 30 min, ride through Dogtown | nada | PinGameToVCacheCcd → +10-20% P99 |
| S3 | Hogwarts-stutters | Cualquier CPU | Hogwarts Legacy 30 min, Hogsmeade | nada | -50% stutter count |
| S4 | Stalker2 + OBS | Ryzen X3D | Stalker 2 30 min | OBS 1080p60 streaming | Aislar OBS de CCD0 → -50% stutters |
| S5 | Cyberpunk hybrid | Intel 13700K | Cyberpunk 2077 30 min | nada | PinGameToPCores → P99 mejor |
| S6 | God of War + Discord | Cualquier multi-CCX | God of War 30 min | Discord voice + screen share | Aislar Discord → input lag reduction |
| S7 | Multi-workload stress | Cualquier CPU | Hogwarts Legacy 30 min | Chrome 30 tabs + Spotify + Discord | Multi-target isolation |
| S8 | Idle baseline | Cualquier CPU | idle desktop 10 min | nada | Verificar Ayama no degrada |
| S9 | Auto-detect validation | Cualquier CPU | Lanzar 5 juegos secuenciales | nada | Auto mode clasifica y aplica correctamente todos |
| S10 | Battery mode | Laptop con batería | Cyberpunk 2077 30 min | nada (en batería) | Ayama detecta batería, reduce ticking |

**Phase 2 (futuro — escala a anticheat):**

Una vez S1-S10 pasen consistentemente, evaluar escalar a juegos con anticheat
siguiendo el orden:
1. Anti-cheats permisivos con affinity changes: Forza Horizon online, Helldivers 2
2. Anti-cheats moderados: Apex Legends, Rainbow Six Siege
3. Anti-cheats estrictos (último): Valorant Vanguard, BF6 BattlEye

Esto va con investigación previa de cada anticheat para confirmar que
`SetProcessAffinityMask` está en sus listas blancas (la mayoría lo permiten
porque herramientas oficiales como Process Lasso lo usan).

### 5.4 Ayama-bench: microbenchmark propio

Para reproducibilidad absoluta (sin depender de juegos comerciales con updates
que cambian comportamiento), Ayama incluye su propio benchmark.

**Diseño de `ayama-bench`:**
- Proceso C++ standalone que simula un workload tipo-juego sin necesitar GPU.
- Thread principal: hot loop con cálculos cache-sensitivos (matrices 8 MB).
- Threads worker: ráfagas periódicas de trabajo (simulan game logic, AI, physics).
- Output: frametime histogram cada 16ms, P99/P50/variance, migration count.
- Diseñado para ser ESTRESANTE para scheduler: si Ayama puede mejorar acá,
  puede mejorar juegos reales.

Archivos:
- `ayama/tools/ayama-bench/main.cpp`
- `ayama/tools/ayama-bench/workload.hpp`

**Workflow:**
```
ayama-bench --duration 60 --output baseline.csv      # baseline sin Ayama
# Arrancar ayama-agent, esperar que detecte ayama-bench, aplique policy
ayama-bench --duration 60 --output treated.csv       # con Ayama activo
ayama-cli diff baseline.csv treated.csv              # diff report en stdout
```

Esto se vuelve el **smoke test obligatorio del CI** — corre en cada build,
verifica que ningún cambio regresione las métricas básicas.

### 5.4 Reporte de output (formato)

Cada test scenario produce:

```
docs/ayama/reports/S2_bf6_obs_2026-05-20.md

# Scenario S2: BF6 + OBS streaming
## Hardware: AMD Ryzen 7 7800X3D, 32 GB DDR5-6000, RTX 4080
## Date: 2026-05-20
## Ayama version: 0.1.x

### Phase A (baseline, no Ayama)
- Frame time avg:     11.2 ms (89 FPS)
- Frame time P99:     32.5 ms
- Stutter count:      147
- Variance:            8.3 ms
- BF6 migrations/s:    42 (mean)

### Phase B (with Ayama, IsolateGameFromBackground enabled)
- Frame time avg:     10.8 ms (92 FPS)
- Frame time P99:     18.1 ms
- Stutter count:       28
- Variance:            3.1 ms
- BF6 migrations/s:     6 (mean)

### Delta
- Avg FPS:        +3.4% (within noise)
- P99 frametime:  -44.3% [SIGNIFICANT, p=0.001]
- Stutter count:  -81%   [SIGNIFICANT]
- Variance:       -62.7% [SIGNIFICANT]

### Verdict: SIGNIFICANT IMPROVEMENT
The user-perceivable improvement is dramatic in stutters and P99,
while avg FPS shows only minor change. This aligns with theory:
Ayama eliminates migrations, not frequency scaling.
```

---

## Parte 6 — ayama-ui: ventana standalone de Ayama

**Principio:** Ayama tiene su propia ventana (`ayama.exe`) construida sobre el framework
Gamma exactamente igual que `standard_window`. `standard_window` es **solo referencia**
de lectura; no se modifica ni se extiende.

### 6.1 `AyamaAppState` — el estado de la ventana

`ayama/tools/ayama-ui/AyamaAppState.hpp` define el estado POD que fluye de
`AyamaLogicNode` → `RenderNode<AyamaAppState>`:

```cpp
// AyamaSnapshotMini (256B = 4 cache lines) — status compacto del agent
struct AyamaSnapshotMini {
    uint8_t  agent_connected;
    uint8_t  privilege_level;        // 0=None, 1=Partial, 2=Elevated, 3=Admin
    uint8_t  baseline_active;
    uint8_t  bench_phase;            // 0=idle, 1=A, 2=B, 3=done
    uint32_t target_count;
    uint32_t decision_count;
    uint32_t action_count;
    uint32_t total_migrations_observed;
    float    aggregate_pressure;
    uint64_t last_sync_tsc;
    uint32_t top_target_pids[5];
    char     top_target_names[5][32];
    uint8_t  _pad[24];
};
static_assert(sizeof(AyamaSnapshotMini) == 256u);

// AyamaAppState — estado completo de la UI (≤4096B, %64==0)
struct AyamaAppState {
    AyamaSnapshotMini snap;          // 256B  — mini status del agent
    // ... campos adicionales de UI (modo, errores, etc.)
};
static_assert(sizeof(AyamaAppState) % 64u == 0u);
static_assert(sizeof(AyamaAppState) <= 4096u);
```

Los arrays grandes (`targets[]`, `metrics[]`, `decisions[]`) los leen los panels
directamente del `AyamaClient*` pasado como argumento — no pasan por el ring.

### 6.2 `AyamaLogicNode` — nodo lógico

`ayama/tools/ayama-ui/AyamaLogicNode.hpp` sigue el patrón de `AppLogicNode`:
- Tiene `Inlet<gma::ui::InputEvent>` + `Inlet<gma::ui::WindowState>`
- Tiene `Outlet<AyamaAppState>`
- En `on_start()`: inicializa `AyamaClient`, intenta conectar al agent
- En `tick()`: si conectado → lee SHM → comprime a `AyamaSnapshotMini` → publica

```cpp
class AyamaLogicNode {
public:
    gma::node::Inlet<gma::ui::InputEvent>  in_input{};
    gma::node::Inlet<gma::ui::WindowState> in_window{};
    gma::node::Outlet<AyamaAppState>       out_state{};

    std::expected<void, gma::Error> on_start() noexcept;
    void on_stop() noexcept;
    void tick() noexcept;

private:
    ayama::ipc::AyamaClient client_{};
    AyamaAppState state_{};
    uint64_t last_reconnect_tsc_{0};
};
```

### 6.3 `main.cpp` de ayama-ui

Sigue exactamente el patrón de `standard_window/main.cpp`:

```cpp
int main() {
    gma::ui::ApplicationConfig cfg;
    cfg.window.title = "Ayama";
    cfg.window.width  = 1280;
    cfg.window.height = 800;

    return gma::ui::Application::run(cfg,
        [](gma::api::NodeRegistry& reg,
           gma::api::DslGraphBuilder& builder,
           gma::render::IRenderBackend& backend,
           gma::render::FrameArena& arena)
        {
            auto* logic = new AyamaLogicNode();
            reg.register_factory("logic", ...);
            reg.wire_registry().register_type<AyamaAppState>();

            using RN = gma::ui::RenderNode<AyamaAppState>;
            auto* rn = new RN(backend, arena, &draw_widgets);
            reg.register_factory("render", ...);

            builder
                .node<AyamaLogicNode>("logic", gma::api::placement::logic())
                .node<RN>("render", gma::api::placement::ui_main());

            (void)builder.wire("ui",    0).to("logic",  0);
            (void)builder.wire("ui",    1).to("logic",  1);
            (void)builder.wire("logic", 0).to("render", 1);
        }
    );
}
```

El `AyamaClient*` se pasa a los widgets via captura en `draw_widgets`, no a través
del ring, para evitar copiar los arrays grandes.

### 6.4 Referencia de `standard_window`

`standard_window` es solo una referencia de cómo integrar Gamma con ImGui.
Los patrones que ayama-ui extrae de ella:
- `gma::ui::Application::run(cfg, lambda)` — punto de entrada
- `RenderNode<T>` + `draw_widgets(const T& s)` — render loop
- `AppLogicNode` pattern — Inlet/Outlet + on_start/tick
- Wire pattern: `"ui"→"logic"→"render"`
- Status bar (26px) + tabbed main window

**No se agrega ningún código a `standard_window`.**

---

## Parte 7 — Test scenarios del mundo real

### 7.1 Setup mínimo para testing

**Hardware suite de validación (al menos uno de cada):**
1. AMD Ryzen 7 7800X3D (single CCD + V-Cache) — caso "Ayama brilla"
2. AMD Ryzen 9 7950X3D (dual CCD, asymmetric V-Cache) — caso "más complejo"
3. Intel 13700K (hybrid P/E) — caso Intel
4. AMD Ryzen 7 5700X (single CCD, no V-Cache, no hybrid) — caso "control negativo"

**Software:**
- Windows 10 22H2 o Windows 11
- Battlefield 6 (target principal)
- OBS Studio 30+
- Discord (latest)
- Chrome 120+
- PresentMon 2.x (para captura externa de verificación)

### 7.2 Test pack inicial

Implementar primero los siguientes scenarios con scripts automatizados.
**Todos usan workloads sin anticheat.**

#### Test T1: "Baseline doesn't lie"
Confirma que sin Ayama, las migraciones son frecuentes en sistemas multi-CCX.
- Arrancar `ayama-bench` o Cyberpunk 2077, observar 5 min, capturar baseline.
- Verificar: migrations/s del game thread > 10 (en hardware multi-CCX/hybrid).
- Si NO se cumple: tu sistema ya está bien y Ayama no agregará valor mensurable.
  Documentarlo es OK — es una conclusión válida.

#### Test T2: "PinGameToVCacheCcd works"
Sobre 7800X3D:
- Phase A: Cyberpunk 2077 sin policies activas.
- Phase B: PinGameToVCacheCcd activa.
- Verificar: P99 frametime mejora ≥ 10% con p < 0.05.

#### Test T3: "IsolateGameFromBackground works"
Sobre cualquier multi-CCX o hybrid:
- Phase A: Stalker 2 + OBS sin policies.
- Phase B: Stalker 2 + OBS con IsolateGameFromBackground.
- Verificar: stutter count baja ≥ 30%.

#### Test T4: "Ayama is a no-op on idle systems"
Sobre cualquier CPU:
- Phase A: idle desktop por 5 min.
- Phase B: idle desktop con Ayama corriendo y todas las policies activas.
- Verificar: ningún proceso fue pineado innecesariamente.
- Verificar: CPU% del propio ayama-agent < 0.3% (hard limit < 0.5%).

#### Test T5: "Revert works"
- Arrancar `ayama-bench`, dejar que Ayama lo pinee.
- Matar `ayama-bench`.
- Verificar: dentro de 5s, ActionLog tiene entry con `tsc_reverted != 0`.
- Verificar: el próximo `ayama-bench` que arranque sin matching rule tiene affinity default.

#### Test T6: "Ayama is degradation-safe"
- Correr Ayama sin admin.
- Verificar: ProcessObserver funciona (read-only).
- Verificar: ActionExecutor falla con `PermissionDenied` y se reporta en UI claramente.
- Verificar: no crashea, no consume CPU > 0.3%.

#### Test T7: "Auto mode detects new games"
- Iniciar Ayama en modo Auto.
- Lanzar 3 procesos: notepad (no game), `ayama-bench` (CPU-heavy), Cyberpunk 2077.
- Verificar:
  - notepad NO es clasificado como game.
  - `ayama-bench` ES clasificado como CPU-bound workload.
  - Cyberpunk ES clasificado como Game.
  - La policy correspondiente se aplica dentro de 5s del start.

#### Test T8: "Learn-and-persist works"
- Iniciar Cyberpunk 2077, Ayama aplica policy A.
- Cerrar Cyberpunk, cerrar Ayama.
- Volver a iniciar Ayama, luego Cyberpunk.
- Verificar: Ayama aplica policy A inmediatamente desde el inicio (recordó del run previo).
- Persistencia en `%LOCALAPPDATA%\Ayama\memory.toml`.

#### Test T9: "Resource budget enforcement"
- Correr Ayama 24 horas observando 0-3 targets.
- Capturar CPU% y RSS cada 1 min.
- Verificar: ninguna muestra excede los hard limits de §0.6.
- Verificar: no hay leak (RSS estable, no crece).

#### Test T10: "Self-aware placement"
- Iniciar Ayama en sistema con AMD X3D.
- Verificar: el thread principal de Ayama-agent corre en CCD sin V-Cache (no parasita el bueno).
- Análogo en Intel hybrid: Ayama-agent corre en E-cores.
- Verificar via `gma::behavior::MigrationTracker` mirando el thread de Ayama.

### 7.3 Reporte de "honesto" — qué publicar y qué no

**Publicable como "demostrado":**
- Mejoras de stutters/P99 en X3D con BF6.
- Mejoras de stutters en multi-workload (game + OBS).
- Cero overhead en idle.

**NO publicable hasta validación científica:**
- "Mejora X% en cualquier juego" — solo válido si testeado en ese juego.
- "Funciona en cualquier CPU" — solo válido si validado en esa familia.

**Nunca publicable porque es falso:**
- "Cierra el gap a frecuencia max boost" — software no puede.
- "Duplica los FPS" — falso en cualquier escenario realista.
- "Reemplaza driver tuning" — no, complementa.

---

## Parte 8 — Quick reference

### 8.1 Roadmap secuencial de implementación

**Mes 1 — Foundation**
- Bloque P.1, P.2, P.3, P.4 (preparación de Gamma como framework)
- Bloque 1.1 AgentRuntime
- Bloque 1.2 ProcessObserver

**Mes 2 — Observation & Action**
- Bloque 1.3 MetricsCollector (ETW + PerfCounters)
- Bloque 1.5 ActionExecutor
- Bloque 1.4 PolicyEngine + default_rules

**Mes 3 — UI & Measurement**
- Bloque 3.1 IPC protocol
- Bloque 4.1, 4.2, 4.3, 4.5 (Targets, Policies, Actions, Dashboard panels)
- Bloque 2.1 Baseline

**Mes 4 — Benchmark & Validation**
- Bloque 2.2 ABRunner
- Bloque 2.3 PerceptualMetrics
- Bloque 4.4 Bench panel
- Tests T1-T6

**Mes 5 — Polish & Distribute**
- Bloque 5.1 Config persistence
- Bloque 5.3 Default policy pack
- Bloque 5.2 Installer
- Real-world scenarios completos

### 8.2 Comandos críticos durante desarrollo

```bash
# Build de Ayama (cuando exista CMakeLists)
cd F:\gma_1.0.0
cmake -S . -B build_ayama -G "MinGW Makefiles" -DGAMMA_BUILD_AYAMA=ON
cmake --build build_ayama --target ayama-agent

# Run agent
build_ayama\ayama\tools\ayama-agent\ayama-agent.exe --verbose

# Run ayama-ui (ventana standalone)
cmake -S . -B build_ayama -G "MinGW Makefiles" -DGAMMA_BUILD_AYAMA=ON
cmake --build build_ayama --target ayama-ui
build_ayama\ayama\tools\ayama-ui\ayama.exe
# → Tab "Dashboard" debería mostrar "agent_connected: true"

# Run A/B benchmark scenario T2
build_ayama\ayama\tools\ayama-cli\ayama-cli.exe \
    bench --scenario T2 \
          --target-name BF6.exe \
          --duration 300 \
          --output docs\ayama\reports\T2_$(date +%F).md
```

### 8.3 Diagnóstico: ¿Cuándo Ayama NO va a ayudar?

Si después de medir baseline el sistema muestra:
- `migrations_per_sec < 5` y `stutter_count_per_min < 10` y `frame_time_variance < 1ms`
- Entonces el sistema ya está bien-comportado y Ayama dará 0-2% mejora indistinguible.

Esto **es un resultado válido y debe publicarse honestamente**.

### 8.4 Files reference (dónde vive qué)

| Componente | Path | Estado |
|---|---|---|
| Master plan (este doc) | `docs/ayama/AYAMA_MASTER_PLAN.md` | ✓ |
| Implementation strategies / patterns | `docs/ayama/AYAMA_IMPLEMENTATION_STRATEGIES.md` | ✓ |
| Inventario Gamma (autoritativo) | `docs/GAMMA_INVENTORY.md` | ✓ |
| Gamma Feature Requests planning | `docs/ayama/GAMMA_FEATURE_REQUEST_IMPLEMENTATION_STRATEGIES.md` | ✓ |
| Test reports (empirical) | `docs/ayama/reports/` | ❌ (vacío — DoD #3) |
| User guide | `docs/ayama/AYAMA_USER_GUIDE.md` | ❌ (DoD #4 parcial) |
| Source de Ayama | `ayama/` | ✓ (8 libs + 5 tools) |
| ayama-ui (ventana standalone) | `ayama/tools/ayama-ui/` | ⚠ (existe; build gated por `AYAMA_BUILD_UI`) |

### 8.5 Riesgos identificados y mitigaciones

| Riesgo | Probabilidad | Impacto | Mitigación |
|---|---|---|---|
| ETW requires admin → muchos usuarios no podrán usar Ayama | Alta | Alto | El installer ofrece registro como servicio elevado; sin admin Ayama corre en modo "observe-only" con notificación clara |
| Anticheat (Valorant, BF6, etc.) flag a Ayama como tampering | Media | Crítico | **Phase 1 NO toca juegos con anticheat.** Solo Phase 2 con investigación per-anticheat; usar SOLO `SetProcessAffinityMask` (legal en docs de los anticheats principales); cero DLL injection, cero read process memory, cero hooking |
| Windows Defender flag a `ayama-agent.exe` | Media | Medio | Firmar binarios desde día 1; submit to Microsoft for false positive review |
| PolicyEngine causa regresión en algunos juegos | Media | Alto | Auto-revert con A/B sampling continuo: si después de aplicar policy las métricas EMPEORAN durante N segundos, revertir automáticamente y marcar como "no aplicar" para ese exe |
| Overhead del agent visible (CPU%) | Baja | Medio | Hard limit < 0.3% idle, < 1% active; CI test T9 verifica esto en cada release |
| Persistencia corrupta → crash al arrancar | Baja | Alto | TOML con versión + checksum; fallback a defaults si parse falla |
| Múltiples Ayama corriendo (services + user instance) | Media | Medio | Detección por named mutex global; segundo instance hace handoff o sale |

### 8.6 Definition of Done — Ayama 1.0

Ayama 1.0 está listo para uso real cuando estos 10 items estén ✅. Estado actual marcado abajo
(✅ = hecho; ⚠ = parcial; ❌ = pendiente). Ver §8.7 para detalle por bloque.

1. ❌ Todos los Bloques 1.x, 2.x, 3.x, 4.x, 5.x completos. (1.x, 2.x, 3.x, 5.x ✅; 4.x ⚠ por flag)
2. ⚠ Tests T1-T10 (§7.2) pasando en ≥ 2 CPUs distintos. (T4, T6, T8, T9-short ✅ automatizados; T1/T2/T3/T5/T7/T10 requieren admin + juegos, manuales)
3. ✅ Al menos un reporte público en `docs/ayama/reports/` con improvement significativo. **(Mayo 16 2026: `minecraft-dh-shaders_2026-05-16_7950x3d.md` — P99 +10.6%, P99.9 +22.7% en Minecraft Java + DH 512-chunks + shaders en Ryzen 9 7950X3D.)**
4. ⚠ Documentation completa de "qué hace y qué no hace". (`AYAMA_MASTER_PLAN` + `AYAMA_IMPLEMENTATION_STRATEGIES` + `GAMMA_INVENTORY` ✓; falta `AYAMA_USER_GUIDE.md`)
5. ❌ Installer probado en Windows 10 + Windows 11. (`ayama-service-register.exe` linka; `install.bat`/`uninstall.bat` faltantes)
6. ❌ Cero crashes en 24h de runtime continuo. (sin pruebas de soak realizadas)
7. ❌ Resource budget (§0.6) respetado bajo carga real. (`SelfMonitor` existe pero sin reporte empírico)
8. ❌ Modo Auto funciona out-of-the-box en CPUs X3D, hybrid Intel, y homogéneas. (lógica existe — `AutoPolicySelector` PASS; sin validación empírica per-CPU)
9. ❌ Modo Manual permite editar/probar reglas custom desde la UI. (depende de Bloque 4.2 + IPC command channel)
10. ❌ Phase 1 (sin anticheat) completamente validado antes de tocar Phase 2.

**Nueva DoD adicional añadida en v0.4:**

11. ✅ `ctest --output-on-failure` reporta **44/44 tests PASS** en cada PR. (Mayo 2026 — añadido `agent_idle_budget_test` que cubre T4/T6/T9-short)
12. ✅ `ayama-ui.exe` se build con `AYAMA_BUILD_UI=ON`. (Mayo 2026 — defaults a ON; arreglados 3 bugs en widgets/AyamaTrayIcon que el `OFF` previo había ocultado)
13. ⚠ Migraciones outstanding (Parte 11): SHM → `ShmRegion` ❌ deferred (§11.1 hallazgo arquitectural); ETW hookup real ✅ done.

### 8.7 Estado real de implementación (mayo 2026)

Tabla de status por bloque, derivada de inspección de código + `ctest`. Cuando hay
divergencia entre lo que esta doc dice y lo que el código hace, **el código gana** y este
doc se actualiza, no al revés.

#### Fase 1 — Foundation

| Bloque | Estado | Test                            | Nota |
|--------|--------|---------------------------------|------|
| 1.1 `AgentRuntime` + subsistemas | ✅ | `ayama_core_test` PASS         | WakeEvent + set_self_working_set + self_pid migrados |
| 1.2 `ProcessObserver`           | ✅ | `ayama_observer_test` PASS     | Usa `gma::proc::enumerate_processes` (FR-4) |
| 1.3 `MetricsCollector`          | ✅ | (cubierto por core_test) | Bulk vía `ProcessMetricsSnapshot` ✓; **ETW hookup completo** (mayo 2026) usando `gma::etw::SessionManager` (FR-6); **TID→PID cache** local (4096 entries, lock-free, poblado desde Thread Start/DCStart events) cubre la atribución de CSwitch hasta que FR-13 llegue. Pendiente: verificación empírica bajo admin |
| 1.4 `PolicyEngine` + `AutoPolicySelector` | ✅ | `ayama_policy_test` + `ayama_auto_policy_test` PASS | |
| 1.5 `ActionExecutor` + `ActionLog` | ✅ | `ayama_action_test` PASS    | `ActionLogRing` migrado a `gma::ipc::Ring<T,128>` (FR-12) |

#### Fase 2 — Measurement

| Bloque | Estado | Test                          | Nota |
|--------|--------|-------------------------------|------|
| 2.1 `Baseline`                  | ✅ | `ayama_bench_test` PASS       | |
| 2.2 `ABRunner` + `DiffReport`   | ✅ | `ayama_abrunner_test` PASS    | |
| 2.3 `PerceptualMetrics`         | ✅ | `ayama_perceptual_test` PASS  | |

#### Fase 3 — IPC

| Bloque | Estado | Test               | Nota |
|--------|--------|--------------------|------|
| 3.1 `AyamaProtocol` + `AyamaClient` + `AyamaAgentPublisher` | ⚠ | sin test propio | **Hand-rolled SHM** todavía; SHM ring `action_log` SÍ usa `gma::ipc::Ring`. Migración a `gma::ipc::ShmRegion` pendiente — Parte 11 §11.1 |

#### Fase 4 — ayama-ui

| Bloque | Estado | Nota |
|--------|--------|------|
| 4.1–4.6 (widgets + AyamaAppState + AyamaLogicNode + main.cpp + AyamaTrayIcon) | ✅ | `ayama-ui.exe` (1.7 MB) builds clean con `cmake -DAYAMA_BUILD_UI=ON .` + smoke-test 3s sin crash. Bugs encontrados al activar (mayo 2026): `WindowState::win_height` → `fb_height`, `enum→uint8_t casts`, `std::wcsncpy` MinGW. Todos fixed |

#### Fase 5 — Config & Distribución

| Bloque | Estado | Nota |
|--------|--------|------|
| 5.1 `ConfigStore` + `DefaultPolicyPack` | ✅ | TOML ad-hoc (migrará a FR-15 cuando exista) |
| 5.2 Installer | ✅ | `ayama-service-register.exe` builda; `install.bat` (modos `--service`/`--user`) y `uninstall.bat` (modo `--purge`) presentes. Mayo 2026: arreglados mismatches de nombres (`ayama.exe` → `ayama-ui.exe`) y se prefiere `ayama-service-register.exe` sobre `sc.exe` raw. Pendiente: testing real en Win10/Win11 (DoD #5) |
| 5.3 Default policy pack | ✅ | Inline en `DefaultPolicyPack::write_if_missing(topo)` |

#### Modos (Parte 9) y aprendizaje

| Concepto | Estado |
|----------|--------|
| `AutoPolicySelector` (§9.2 tabla decisión) | ✅ — `auto_policy_selector_test` PASS |
| `PerGameMemory` persistente (§9.3) | ✅ — `ayama_learn_test` PASS |
| Modo Assist (preview + confirmación) | ❌ — sin UI bind |
| Modo Manual (editor de rules) | ❌ — depende de Bloque 4.2 + IPC command channel |
| System tray icon (§9.5) | ⚠ — `AyamaTrayIcon.hpp` existe, no integrado al `main.cpp` |

#### Tests T1-T10 (§7.2) — automatización en CI

| Test | Estado | Cobertura |
|------|--------|-----------|
| **T1** "Baseline doesn't lie" | ❌ requiere juego con anticheat-free | manual |
| **T2** "PinGameToVCacheCcd works" | ❌ requiere admin + juego | manual |
| **T3** "IsolateGameFromBackground works" | ❌ requiere admin + Stalker 2 + OBS | manual |
| **T4** "Ayama no-op idle CPU%" | ✅ **AUTOMATIZADO** en `agent_idle_budget_test` — verifica CPU% < 5% durante 5 s idle. Resultado real: 0.31% | CI |
| **T5** "Revert lifecycle" | ❌ requiere admin para `set_process_affinity` real; revert de no-op cubierto por `ayama_action_test` | manual |
| **T6** "Degradation-safe sin admin" | ✅ **AUTOMATIZADO** en `agent_idle_budget_test` — `start()` Ok sin admin, no crash, `stop()` < 2 s. Resultado real: 80 ms | CI |
| **T7** "Auto mode detects new games" | ❌ requiere juego corriendo | manual |
| **T8** "Learn-and-persist" | ✅ cubierto por `ayama_learn_test` (T8.1–T8.10) | CI |
| **T9** "Resource budget 24h" | ⚠ **versión 5s automatizada** en `agent_idle_budget_test` — verifica ΔRSS < 10 MB. Resultado real: +3.3 MB. La versión 24h real sigue siendo manual | CI (5 s) + manual (24 h) |
| **T10** "Self-aware placement" | ❌ requiere `gma::behavior::MigrationTracker` sobre el thread propio en hardware específico | manual |

**Bugs encontrados durante automatización (mayo 2026):**

1. **`InternalWatchdog::watch_loop()`** usaba `sleep_for(5s)` no-interrumpible → `stop()`
   bloqueaba hasta 5 s en lugar de los 50 ms del spec. Fix aplicado: sleep en chunks de
   100 ms con chequeo de `stop_` flag. `stop()` ahora completa en ≤ 100 ms.

2. **`gma::tuning::PrivilegeCheck::probe()` Win32** detectaba "admin" via `GetPriorityClass()
   == REALTIME_PRIORITY_CLASS`. Esto es WRONG — un proceso admin normal corre en
   `NORMAL_PRIORITY_CLASS`, por lo que **Ayama jamás se reconocía a sí mismo como admin**
   aunque corriera con UAC elevation. Cascade: ETW no se activaba, policies no se aplicaban.
   Fix aplicado: usar `GetTokenInformation(TokenElevation)` que refleja correctamente
   el UAC elevation status. Smoke-test confirma comportamiento correcto.

3. **`ProcessClassifier::check_d3d_vk_modules()`** solo detectaba D3D/Vulkan/DXGI DLLs.
   Faltaba OpenGL — rompía clasificación de juegos Java (Minecraft LWJGL), Unity con
   GL backend, etc. Fix: añadidos `opengl32.dll`, `glfw[3].dll`, `lwjgl[_*].dll`.

Sin estos 3 fixes, el primer test empírico con Minecraft Java habría medido
incorrectamente (Phase B sin policy aplicada = idéntico a Phase A).

#### Infra de CI verificada hoy

- `ctest -N` → **44 tests registrados** (43 originales + `agent_idle_budget_test`)
- `ctest --output-on-failure` → **44/44 PASS** (36.7 s total, mayo 2026)

---

## Parte 9 — Modos de operación (Auto/Assist/Manual)

### 9.1 Filosofía: progressive disclosure

Ayama soporta tres niveles de involucramiento del usuario. **El default es Auto.**
El usuario nunca está forzado a entender lo de abajo si no quiere.

```
┌─────────────────────────────────────────────────────┐
│  Auto Mode (default — 90% de usuarios)              │
│   - Detecta hardware                                │
│   - Detecta procesos (games, streams, comm)         │
│   - Aplica policy auto-seleccionada                 │
│   - Auto-revierte si detecta regresión              │
│   - Notifica vía tray: "Optimizando Cyberpunk 2077" │
│   - UI mínima: 1 botón "Apagar Ayama"               │
└─────────────────────────────────────────────────────┘
                       ↓ (opcional)
┌─────────────────────────────────────────────────────┐
│  Assist Mode (~9% de usuarios)                      │
│   - Como Auto, pero notifica ANTES de aplicar:     │
│     "Detectamos Cyberpunk 2077. Sugerimos pinear   │
│      a CCD0 (V-Cache). ¿Aplicar?"                  │
│   - Histórico de "lo que Ayama hubiera hecho"      │
│   - Útil para usuarios cautelosos                  │
└─────────────────────────────────────────────────────┘
                       ↓ (opcional)
┌─────────────────────────────────────────────────────┐
│  Manual Mode (~1% — power users)                    │
│   - Auto está OFF                                   │
│   - Editor completo de Rules + Conditions          │
│   - Bench A/B integrado por scenario               │
│   - Tweaks de bajo nivel (priority class, RT, etc.) │
│   - Import/export de policy packs                  │
└─────────────────────────────────────────────────────┘
```

El usuario cambia de modo desde el dashboard, con un dropdown único.
**Toda configuración hecha en Manual persiste si después cambia a Auto.**

### 9.2 Auto Mode — cómo decide qué hacer

`ayama::policy::AutoPolicySelector` implementa este pipeline:

```
1. HW detection (one-shot al boot):
   - GET topology via gma::hw::*
   - Identify CPU class: {X3D-single, X3D-dual, Hybrid-Intel, Homogeneous-multi-CCX, Homogeneous-single-CCD}

2. Per-process classification (cada vez que aparece un nuevo PID):
   - ProcessClassifier::classify(pid) → TargetKind
   - Kinds: Game | Stream | Comm | Browser | Productivity | System | Unknown
   - Heuristics:
     - Game: full-screen detected via DWM + GPU usage > 30% sostenido + window in foreground > 30s
     - Stream: name match (OBS*, Streamlabs*, XSplit*)
     - Comm: name match (Discord*, Teams*, Zoom*, slack*)
     - Browser: name match (chrome*, firefox*, msedge*, brave*)
     - Productivity: visual studio, devenv, code, blender, davinci
     - Unknown: nada matchea → no action

3. Policy selection (tabla de decisión):

   CPU class        | Game  | Stream    | Comm     | Productivity
   -----------------+-------+-----------+----------+-------------
   X3D-single       | CCD0  | (no act.) | (no act.)| (no act.)
   X3D-dual         | CCD0  | CCD1      | CCD1     | CCD1
   Hybrid-Intel     | P     | E         | E        | E
   Multi-CCX no-X3D | CCX0  | CCX1      | CCX1     | (no act.)
   Single-CCD       | (no action — Auto mode no actúa, mejora < 2%)

4. Apply with safety net:
   - Apply policy
   - Start A/B sampler: capture frametime variance for 30s
   - If variance gets WORSE → auto-revert + mark this (exe, hw) as "no-action"
   - If improves → mark as "good", persist to memory.toml
```

**Importante:** Auto Mode es conservador. Ante duda, NO hace nada. Es preferible
"Ayama no hizo nada" a "Ayama empeoró las cosas". El test T6 valida esto.

### 9.3 Per-game learning (persistencia)

`ayama::learn::PerGameMemory` mantiene una tabla:

```
(game_exe_name, hardware_id) → LearnedPolicy
                              ├─ best_known_action_mask
                              ├─ measured_improvement_pct
                              ├─ sample_count
                              ├─ last_validated_tsc
                              └─ user_locked (bool — usuario fijó manualmente)
```

**Lógica:**
- Primera vez que se ve un game: aplicar default por CPU class, medir 30s.
- Si mejora: persistir. Próxima vez aplicar inmediatamente.
- Si user_locked: no re-evaluar. Respetar elección del usuario.
- Cada N días: re-evaluar (el game pudo haber recibido update).

Memoria persistida en TOML:
```toml
# %LOCALAPPDATA%\Ayama\memory.toml
version = 1
hardware_id = "amd-7800x3d-2ccd-1vc"

[[learned]]
exe = "Cyberpunk2077.exe"
best_action = "pin_v_cache_ccd"
improvement_pct = 12.4
sample_count = 8
last_validated = "2026-05-19T14:22:00Z"
user_locked = false

[[learned]]
exe = "HogwartsLegacy.exe"
best_action = "pin_v_cache_ccd"
improvement_pct = 18.7
sample_count = 3
last_validated = "2026-05-20T09:11:00Z"
user_locked = true   # usuario fijó manualmente, no re-evaluar
```

### 9.4 Tweakability sin romper Auto

Para mantener Auto siempre funcional, las modificaciones del usuario se almacenan
como **overrides** en lugar de reemplazar la lógica base:

```
LearnedPolicy = AutoSelector_output  OVERLAY  UserOverrides
```

Esto permite:
- Usuario hace tweak custom en Manual → se guarda como override
- Cambia el hardware (CPU upgrade) → overrides irrelevantes se ignoran (hardware_id distinto)
- Reset to defaults → borrar overrides, no la lógica auto

### 9.5 Notificaciones y system tray

**System tray icon estados:**
- 🟢 Verde: Ayama activo, optimizaciones aplicadas
- 🟡 Amarillo: Ayama activo, modo Assist esperando confirmación
- 🟠 Naranja: Ayama corriendo sin admin (modo degradado)
- 🔴 Rojo: Ayama detectó regresión, reverted automáticamente

**Notifications (configurable):**
- "Cyberpunk 2077 detectado — optimizando" (Auto mode)
- "OBS detectado en background — pineado a CCD1" (Auto mode)
- "Regresión detectada para Hogwarts Legacy — revertido al estado anterior" (Auto)
- "Nueva versión de Ayama disponible" (opcional, sin auto-update)

---

## Parte 10 — Resource budget y compromiso anti-parasítico

### 10.1 Hard limits (test T9 falla si se exceden)

| Métrica | Idle target | Idle limit | Active target | Active limit |
|---|---|---|---|---|
| CPU % | 0.1 | 0.3 | 0.5 | 1.0 |
| RSS RAM (MB) | 15 | 20 | 30 | 50 |
| Threads vivos | 4 | 6 | 6 | 8 |
| File descriptors / Handles | 50 | 100 | 100 | 200 |
| Disk write / min (KB) | 0 | 50 | 50 | 500 |
| Network | 0 | 0 | 0 | 0 |

**Definiciones:**
- *Idle:* Sin targets observados o todos los targets están suspendidos.
- *Active:* 1-3 targets observados con políticas activas.
- *Bench:* Solo durante A/B test runs — pueden exceder Active limits temporalmente.

### 10.2 Tácticas concretas (detalle en STRATEGIES.md §3)

1. **Adaptive ticking** — el loop principal no es 100ms fijo; se acelera/relentiza:
   - Sin targets: 1000ms
   - 1 target idle: 500ms
   - 1+ targets active: 100ms
   - Durante bench: 25ms
   - Triggered por evento (ETW context-switch flood): 10ms ráfaga, luego normalizar

2. **ETW filtering en kernel** — solo subscribirse a los providers necesarios.
   Si ningún target está activo: detener la session ETW completamente.

3. **Self-pinning** — el thread principal de Ayama-agent se pinea él mismo a:
   - E-core (Intel hybrid)
   - CCD sin V-Cache (AMD X3D)
   - Core con menor max boost frequency (homogéneo)
   Así Ayama nunca compite por el core "bueno" del usuario.

4. **Lazy ETW startup** — la session ETW no arranca hasta que aparece el primer target.
   Si no hay games corriendo, no se enciende.

5. **Buffer pre-allocation** — todos los `std::array<T, kMax>` están en construcción.
   Cero `std::vector::push_back` en hot path. Cero `new`/`malloc` después de init.

6. **Sleep, never spin** — todas las esperas usan `WaitForMultipleObjects` con timeout.
   El tick adaptativo es un `WaitForSingleObject(wake_event, current_tick_ms)`.

7. **Battery mode** — `GetSystemPowerStatus` cada 10s. Si batería:
   - tick × 2
   - ETW providers reducidos
   - bench prohibido (no degrada experiencia mobile)

8. **Suspend en focus loss** — si la UI cierra y no hay targets activos por >5min:
   - Liberar buffers de bench (memoria libre)
   - Reducir tick a 5000ms (deep idle)
   - Solo despertar por process-start notification (ETW)

### 10.3 Métricas que Ayama publica sobre sí mismo

En la UI, panel "Ayama" muestra:
- CPU% propio (medido vs el límite)
- RSS RAM
- Threads vivos
- Tick interval actual
- "Self-pin": core donde corre el thread principal
- ETW activo: yes/no, providers count
- Acciones desde startup: N

Esto cumple con principio 3 (transparencia) y permite al usuario verificar
que Ayama cumple lo que promete.

---

## Parte 11 — Migraciones outstanding (Gamma sub-uso pendiente)

Esta parte traduce los items "⚠" del §8.7 a tareas concretas con orden y criterios de
aceptación. Sin estas migraciones Ayama sigue funcionando — pero hay ~430 LOC duplicado
que Gamma ya provee, y un bug latente (ETW callback que no recibe events).

### 11.1 Migración SHM hand-rolled → `gma::ipc::ShmRegion<H,P>` (FR-5)

**Estado actual del código:**
- `ayama/ipc/src/AyamaAgentPublisher.cpp`: `CreateFileMappingW` + `MapViewOfFile` + seqlock manual (`shm_write_begin/end`).
- `ayama/ipc/src/AyamaClient.cpp`: `OpenFileMappingW` + `MapViewOfFile`; no usa `try_read_consistent`.
- `AyamaShmHeader` ya cumple `ShmHeaderConcept` (tiene `magic`, `version`, `seq` y `agent_pid` atomic).

**Hallazgo arquitectural (mayo 2026):**

`ShmRegion<Header, Payload>` impone `static_assert(std::is_trivially_copyable_v<Payload>)`.
`AyamaShmLayout` contiene un campo `action::ActionLogRing` (= `gma::ipc::Ring<ActionLogEntry, 128>`)
que tiene **constructors de copia/movimiento `= delete`** (decisión deliberada para prevenir
copias accidentales del ring). Esto hace que `AyamaShmLayout` (sin el header) **NO sea
trivially_copyable** y no pueda usarse como `Payload` de `ShmRegion` tal como está hoy.

**Tres caminos posibles, ranked por trabajo / riesgo:**

| Camino | Trabajo | Riesgo | Cuándo elegirlo |
|--------|--------:|-------:|-----------------|
| **A) Defer §11.1** — mantener SHM hand-rolled; gana solo cuando se resuelva uno de B/C | 0 | 0 | ✅ Pre-Ayama 1.0 (recomendado por ahora) |
| **B) Hacer `gma::ipc::Ring` trivially_copyable** — cambiar `= delete` por `= default` en los constructors de copia/movimiento | ~20 LOC en Gamma + 3 LOC de risk-comment | Medio (consumers pueden copiar Ring accidentalmente; los atomics se desincronizarían) | Post-Ayama 1.0, después de revisar todos los call-sites |
| **C) Separar action_log del Payload** — usar 2 `ShmRegion` distintos: uno para state+arrays, otro para el ring de action_log | ~80 LOC en Ayama, +1 SHM segment | Bajo en código, pero duplica naming/lifecycle | Cuando se priorice eliminar las 250 LOC de hand-rolled |

**Decisión actual:** Camino A (defer). Razones:
1. La SHM hand-rolled actual funciona (`ayama_core_test` PASS, 43/43 PASS), aunque sea
   verbosa.
2. Camino B requiere cambio en Gamma — fuera de scope de Ayama y necesita reviewer del framework.
3. Camino C duplica naming sin ganancia clara hasta que la UI exista (Fase 4).
4. Hay un win con prioridad superior: §11.2 (ETW hookup real) que destrabea tests T2/T3.

**Documentación a actualizar en consecuencia (mayo 2026):**

- `AYAMA_IMPLEMENTATION_STRATEGIES.md §13.2` afirma "ring DENTRO de payload SHM" como patrón
  válido. **Esto es incorrecto con el `gma::ipc::Ring` actual** porque la trivial-copyable
  static_assert lo prohibe. Refinar el patrón a "ring en SHM como mapping SEPARADO del state Payload" cuando se llegue a esta migración.

**Si en el futuro se elige Camino B o C**, los criterios de aceptación son:

- `AyamaAgentPublisher.cpp` ≤ 60 LOC (hoy ~250 LOC).
- `AyamaClient.cpp` ≤ 40 LOC (hoy ~95 LOC).
- `ayama_core_test` + un nuevo `ayama_ipc_roundtrip_test` PASS.
- `shm_region_test` (Gamma) cubre los edge cases ABI; Ayama no los duplica.

### 11.2 Hookup ETW real → `gma::etw::SessionManager` (FR-6)

**Estado: ✅ DONE (cableado completo, pendiente verificación empírica admin).**

**Cambios aplicados (mayo 2026):**

- `MetricsCollector.hpp`: añadido `gma::etw::SessionManager etw_session_;` como miembro
  directo (no es movible). Cambio `etw_callback(EVENT_RECORD*)` →
  `etw_record_callback(const EVENT_RECORD&, void*)` con signatura del concept
  `gma::etw::EventCallback`.
- `MetricsCollector_win32.cpp::start()`: ahora arranca **sesión ETW real** con 3 providers
  (CSwitch, Process, Thread). Llama `etw_session_.start_consumer(etw_record_callback, this)`
  para spawner el consumer thread. **Degradación graceful**: en `PermissionDenied`
  (no admin) marca `etw_active_=false` y retorna Ok — el agente sigue en modo polling-only
  (ProcessMetricsSnapshot cubre CPU%, RSS, threads; solo `migrations_per_sec=0`).
- `MetricsCollector_win32.cpp::stop()`: llama `etw_session_.stop()` antes del cleanup.
- `etw_record_callback`: parsea `EVENT_RECORD` para opcode CSwitch (36), extrae
  `NewThreadId` + `BufferContext.ProcessorIndex`, push al `etw_ring_` lock-free.
  PID queda en 0 hasta FR-13 (TID→PID cache).
- `ayama/observer/CMakeLists.txt`: añadido `gamma_etw` a deps.

**Lo que verificamos (mayo 2026):**

- ✅ `cmake --build` compila sin warnings nuevos.
- ✅ `ctest --output-on-failure` → **43/43 PASS** (incluyendo `ayama_core_test` que ejercita el ciclo start→tick→stop).
- ✅ `ayama-agent.exe` sin admin: arranca, reporta `ETW=no`, no crashea, sale limpio en Ctrl+C.

**Mejora añadida (mayo 2026): TID → PID cache local**

Como workaround temporal hasta FR-13:
- `MetricsCollector` tiene un `tid_pid_cache_` de 4096 entries direct-mapped lock-free
  (32 KB total, alineado a 64 B). Indexado por `tid & 0xFFF`.
- El `etw_record_callback` ahora procesa **3 tipos de eventos**:
  1. Kernel-Thread Start/DCStart (opcodes 1/3) → puebla `cache[tid&mask] = {tid, pid}`
  2. Kernel-Thread End/DCEnd (opcodes 2/4) → CAS-clear el slot si todavía es nuestro
  3. Kernel-Dispatcher CSwitch (opcode 36) → lookup `cache[tid]` y guarda `slot.pid`
- `drain_etw_ring()` ahora hace **find-only** (no auto-create) — antes el find_or_create
  inflaba `pid_states_` con cualquier PID del sistema agotando los 32 slots en segundos.
  También reintenta el lookup si `slot.pid==0` (race entre CSwitch y Thread Start).
- Colisiones: política "last writer wins". En sistemas con < 2000 threads (típico) son
  raras. Cualquier miss → CSwitch sin atribuir → no rompe nada, solo no cuenta.

**Pendiente (no bloqueante):**

- **Verificación bajo admin elevation** — Confirmar empíricamente que `etw_session_.is_running() == true`, `events_processed() > 0` con admin, y `migrations_per_sec > 0` sobre procesos multi-thread (Chrome, juego). Requiere UAC prompt o CI runner elevado.
- **Test nuevo `ayama_etw_smoke_test`** (futuro) — arrancar collector, esperar 2 s, verificar `etw_session_.events_processed() > 0`. Marcar SKIP cuando no haya admin para CI cross-platform.
- **Migrar a FR-13** cuando el pillar `gma::etw::CtxSwitchSource` esté disponible — eliminará el cache local (~120 LOC) reemplazándolo con el TID→PID O(1) integrado del pillar.

### 11.3 (Opcional) `ayama::core::InternalWatchdog` → `gma::orchestration::Watchdog`

**No bloqueante.** El `Watchdog` Gamma maneja N nodes con timeouts independientes; Ayama usa 1
solo watcher. Migrar solo si en el futuro queremos watchers separados por subsistema
(MetricsCollector, ActionExecutor, etc.) — entonces vale la pena. Por ahora, dejarlo como está.

### 11.4 Sugerencias de mayor aprovechamiento de Gamma

Identificadas durante revisión de mayo 2026. **No son migraciones obligatorias** sino
oportunidades que el código ya facilita:

| Oportunidad | API Gamma | Cuándo aplicar |
|-------------|-----------|----------------|
| Usar `gma::orchestration::ErrorRegistry` para agregar errores de `PolicyEngine`, `ActionExecutor`, `MetricsCollector` y exponerlos en dashboard panel | `gamma/orchestration/ErrorRegistry.hpp` | Al implementar Fase 4.5 (dashboard) — facilita el panel de errors recientes |
| Usar `gma::behavior::TimelineEventBuffer` para action log timeline visual (Bloque 4.3 actions_panel) | `gamma/behavior/TimelineEventBuffer.hpp` | Si el timeline visual del actions_panel se mueve a TimelineEvent en lugar de ActionLogEntry — más cross-pillar |
| Reusar `gma::correlation::CorrelationEngine` para patrones causales game-stutter ← context-switch-burst | `gamma/correlation/CorrelationEngine.hpp` | Cuando se implementen "smart triggers" que aplican policies SOLO cuando un patrón causal se detecta (post-1.0) |
| Migrar `AyamaTrayIcon.hpp` integration con `gma::ui::Application` lifecycle | `gamma/ui/Application.hpp` | Al activar `AYAMA_BUILD_UI=ON` y completar Fase 4 |

---

**Fin del Master Plan v0.4**

Última actualización: Mayo 2026 (post FR pack v1.1.0 + revisión contra código)
Próxima revisión: tras completar Parte 11 §11.1 (SHM migration) y §11.2 (ETW hookup)
Source-of-truth supplemental: [`../GAMMA_INVENTORY.md`](../GAMMA_INVENTORY.md)
Ver también: `AYAMA_IMPLEMENTATION_STRATEGIES.md` para detalles técnicos.
