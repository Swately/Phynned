> **📦 Archived planning document — historical.**
> From Phyriad's pre-rebrand era (the project was codenamed *gamma* / *gma*).
> Published for transparency about how the project was actually built — **not**
> a current specification. Version numbers and performance claims here
> (including any "vs Folly" / "production-ready" framing) reflect the project's
> state at the time of writing and may since have been revised or **retracted**;
> the project has reset to `v0.1.0-experimental`. For current status see the
> repo `README.md` and `docs/planning/`. For benchmark-claim validation status
> see `docs/planning/TEST_INVENTORY.md`.

# Ayama — Showcase Master Plan
## Transición de proyecto interno → carta de presentación pública de Gamma Framework

**Version:** 1.0 — Mayo 2026
**Foundation:** Gamma Framework release line 1.1.0 + Ayama Phase B.2 (post CCD Load Defense)
**Status del plan:** Aprobado para implementación tras finalización Phase B.2 (CCD Load Defense done sin stubs).
**Documento hermano:** [`AYAMA_SHOWCASE_IMPLEMENTATION_STRATEGIES.md`](AYAMA_SHOWCASE_IMPLEMENTATION_STRATEGIES.md) — patrones técnicos para máximo rendimiento.
**Documentos relacionados:**
- [`AYAMA_MASTER_PLAN.md`](AYAMA_MASTER_PLAN.md) — plan de implementación core (v0.4)
- [`AYAMA_IMPLEMENTATION_STRATEGIES.md`](AYAMA_IMPLEMENTATION_STRATEGIES.md) — patrones core (v0.3)
- [`reports/EMPIRICAL_EVIDENCE_SUMMARY.md`](reports/EMPIRICAL_EVIDENCE_SUMMARY.md) — 14 reports empíricos
- [`reports/halo-mcc-catalog_2026-05-17_7950x3d.md`](reports/halo-mcc-catalog_2026-05-17_7950x3d.md) — el showcase principal de datos

---

## Tabla de contenidos

- [§0 — Visión y posicionamiento](#0--visión-y-posicionamiento)
- [§1 — Tier 0: Repo hygiene + distribución](#1--tier-0-repo-hygiene--distribución)
- [§2 — Tier 1: Empirical evidence differentiator](#2--tier-1-empirical-evidence-differentiator)
- [§3 — Tier 2: Gamma como framework standalone](#3--tier-2-gamma-como-framework-standalone)
- [§4 — Feature A: Compute Graph del agent](#4--feature-a-compute-graph-del-agent)
- [§5 — Feature B: Topology Map UI](#5--feature-b-topology-map-ui)
- [§6 — Secuencia y dependencias](#6--secuencia-y-dependencias)
- [§7 — Deliverables y criterios de éxito](#7--deliverables-y-criterios-de-éxito)
- [§8 — Out-of-scope (features deferred)](#8--out-of-scope-features-deferred)

---

## §0 — Visión y posicionamiento

### §0.1 — Premisa estratégica

**Ayama no compite con Process Lasso ni con Game Mode de Windows.** Ayama es la **demo application de Gamma Framework** — su valor primario es validar empíricamente que Gamma puede infraestructura de sistema no-trivial, no capturar mercado de gaming.

Reframing aceptado tras análisis honesto:
- Como producto comercial standalone: NO viable (Process Lasso domina, AMD/MS comen el espacio gratis).
- Como **showcase de Gamma**: SÍ viable y estratégicamente correcto.
- Como **contribución de investigación**: ALTO valor (14 reports empíricos publicables, modelo predictivo falsificable).

### §0.2 — Audiencia objetivo del showcase

Tres audiencias en orden de prioridad:

1. **Developers** que evalúan frameworks para sus propios projects. Quieren ver Gamma como dependency upstream. Necesitan API docs, examples, install instructions.
2. **Hardware reviewers / benchmark engineers** que pueden usar Ayama como herramienta de medición y promover Gamma indirectamente.
3. **Power users / X3D enthusiasts** que pueden adoptar Ayama como complemento a Process Lasso. Necesitan installer, FAQ, hardware compatibility list.

Audiencia NO objetivo (explícitamente):
- Mass-market gamers (no pueden ofrecer feedback técnico, esperan instalar-y-olvidar).
- Empresas Fortune 500 (Ayama no escala a server farms, requiere admin).
- Anti-cheat compatibility seekers (out of scope hasta Safe Mode, fase futura).

### §0.3 — Métricas de éxito post-publicación

Métricas verificables 30 días post-release:

| Métrica | Target mínimo | Target ideal |
|---|---|---|
| GitHub stars | 100 | 500 |
| Issues abiertos por terceros (no spam) | 10 | 50 |
| Forks con commits propios | 3 | 15 |
| Menciones en /r/Amd, hardware fora | 5 | 25 |
| Pull requests de comunidad | 1 | 5 |
| Downloads del release zip | 500 | 5000 |

Si en 90 días no se alcanzan los **target mínimos**, el showcase no logró tracción y se reconsidera la estrategia (pivot a paper académico únicamente, o abandono).

### §0.4 — Restricción inquebrantable: máximo rendimiento

Todos los items de este plan **deben preservar** el resource budget actual de Ayama:

- `< 3%` CPU sustained agent (incluyendo nuevas features)
- `< 50` MB RSS agent
- `< 60` FPS draw cost UI
- `< 100` µs overhead por tick añadido por cualquier feature

Cualquier feature que viole esto en steady state es bloqueante para release. Las técnicas para preservarlo están en `AYAMA_SHOWCASE_IMPLEMENTATION_STRATEGIES.md`.

---

## §1 — Tier 0: Repo hygiene + distribución

### §1.1 — Objetivo

Hacer que un developer que llega al repo en GitHub piense "esto es serio" en `<` 30 segundos. Hoy, el repo no es publicable: no tiene README, license, CI, ni binarios pre-built.

### §1.2 — Items concretos

| # | Item | Esfuerzo | Bloquea release? |
|---|---|---|---|
| T0.1 | `git init` + comprehensive `.gitignore` | 1 hora | Sí |
| T0.2 | `LICENSE` (Apache 2.0 recomendado) | 30 min | Sí |
| T0.3 | Root cleanup: mover `_legacy/`, `build*/`, `*.log` | 2 horas | Sí |
| T0.4 | `README.md` con hero stats + screenshot Halo CE | 1 día | Sí |
| T0.5 | GitHub Actions CI: Windows MSVC + MinGW builds | 4 horas | Sí |
| T0.6 | Release zip generation (binarios + docs) | 2 horas | Sí |
| T0.7 | PowerShell install script (idempotente) | 1 día | Sí |
| T0.8 | `CONTRIBUTING.md` + `CODE_OF_CONDUCT.md` | 1 hora | No |
| T0.9 | Traducción dev docs Esp→Eng | 2 días | Sí |
| T0.10 | `FAQ.md` (anti-cheat, hardware support, comparison) | 1 día | Sí |

**Total Tier 0**: ~1 semana de trabajo enfocado.

### §1.3 — Decisiones difíciles ya tomadas

- **License**: Apache 2.0 (no MIT). Razón: provee defensa explícita de patentes, mejor para framework upstreaming. Compatible con BSD-license downstream.
- **CI matrix**: Solo Windows (no Linux build CI todavía). Razón: Linux paths en Gamma son stubs, no validados. Tier 2 puede agregar Linux CI cuando un pillar tenga implementación Linux real (probablemente `topology`).
- **`_legacy/`** (98 MB): mover a `.gitignore` y NO commitear. Si user necesita histórico, backup zip externo basta. No vale el peso en repo público.
- **`build*/` dirs** (varios): nunca commitear, todos en `.gitignore`. CMake regenera de cero.
- **`agent_*.log`** (varios MB cada uno): nunca commitear. `.gitignore` excluye `*.log`.
- **Idioma docs**: README + FAQ + CONTRIBUTING en **inglés** (audiencia internacional). Reports y deep dev docs pueden quedar en mezcla esp/inglés (audiencia más nicho, español es válido para research notes).

### §1.4 — Performance considerations Tier 0

- **CI build speed**: usar GitHub Actions cache para CMake build dir + Gamma artifacts. Target: full build `<` 5 min, incremental `<` 90 s.
- **Release zip size**: target `<` 20 MB final. Excluir docs internos, debug symbols (en release build con `-O3`), test binaries.
- **Install script**: PowerShell paralelizar download de binarios + descarga de PresentMon. Target: 60s end-to-end en conexión 50 Mbps.
- **Repo clone speed**: target `<` 30 s en conexión típica (depende del peso final post-cleanup).

### §1.5 — Salida verificable

Un evaluador externo debería poder:

1. `git clone <repo>` (← `<` 30 s)
2. `cd ayama && powershell -File install.ps1` (← `<` 60 s)
3. Ver UI corriendo con un juego activo
4. Ejecutar un bench A/B/A/B/A
5. Leer el reporte generado

Sin contactar al autor. Sin instalar nada más que el script. Sin abrir issue.

---

## §2 — Tier 1: Empirical evidence differentiator

### §2.1 — Objetivo

Posicionar el catálogo de 14 reports empíricos como **el único valor que ningún competidor tiene**. Bitsum/AMD/MS no publican datos estadísticamente rigurosos sobre sus optimizers. Ayama sí.

### §2.2 — Items concretos

| # | Item | Esfuerzo |
|---|---|---|
| T1.1 | Polish `EMPIRICAL_EVIDENCE_SUMMARY.md` → versión pública | 0.5 día |
| T1.2 | Generar hero plot (Halo MCC curve) como PNG checkeado | 0.5 día |
| T1.3 | Generar comparison chart (Ayama vs PL vs Game Mode vs baseline) | 1 día |
| T1.4 | Escribir `REPRODUCING_BENCHMARKS.md` (paso a paso) | 1 día |
| T1.5 | Polish reports individuales (ortografía, formato consistente) | 1 día |

**Total Tier 1**: 3-4 días.

### §2.3 — El hero plot

Especificación visual del gráfico principal (debe aparecer en README + landing page):

- Eje X: año de release del game engine (2001 → 2024)
- Eje Y izquierdo: Δ Avg FPS (%) — barras verticales
- Eje Y derecho: hot thread CPU share (%) — línea overlaid
- 14 puntos de data, etiquetados con nombre del game
- Halo CE (+98%) destacado en color distinto
- Línea de regresión + intervalo de confianza opcional

Producido con matplotlib en script Python checkeado en `docs/figures/` para regeneración.

### §2.4 — La comparison page

Comparativa empírica honesta vs los 3 alternativos principales. Tabla cuando aplicable:

| Game | Baseline | + Win11 Game Mode | + Process Lasso | + Ayama |
|---|---:|---:|---:|---:|
| Halo CE | 234 | (medir) | (medir) | 463 |
| Halo 3 | 394 | (medir) | (medir) | 470 |
| ... | ... | ... | ... | ... |

**Restricción metodológica importante**: las comparaciones contra PL solo pueden hacerse en máquinas con licencia PL legítimamente adquirida. No bundle. No reverse-engineering. La página puede ofrecer "tu propio kit comparativo: instala PL, corre estos comandos, llena la tabla".

### §2.5 — Performance considerations Tier 1

- **Plot generation script**: matplotlib backend `Agg` para no requerir display. Tiempo total `<` 5 s.
- **Comparison harness** (futuro): mismo bench runner, solo cambia el "estado del sistema" entre runs (PL on/off, etc.).
- **Static assets**: PNG hero plot `<` 100 KB con compresión optimizada. SVG opcional para tooltips interactivos en GitHub Pages.

### §2.6 — Salida verificable

GitHub README muestra:
- Hero plot embebido (visible sin scroll)
- Tabla comparativa con números reales (al menos baseline + Ayama; PL/Game Mode marcados como "user-contributable")
- Link a `REPRODUCING_BENCHMARKS.md`
- Disclaimer honesto: "These numbers are from a single 7950X3D + 4090 system. Your mileage may vary. Reproduce locally using the linked guide."

---

## §3 — Tier 2: Gamma como framework standalone

### §3.1 — Objetivo

Separar Gamma de Ayama suficientemente para que un developer pueda `find_package(Gamma 1.1.0)` en su propio CMake project, sin clonar Ayama. Esto **es** la promoción de Gamma — sin esto, Gamma se ve como "código interno de un app, no framework".

### §3.2 — Items concretos

| # | Item | Esfuerzo |
|---|---|---|
| T2.1 | CMake split: `gamma` como standalone install target | 2 días |
| T2.2 | `examples/` dir con demo por pilar (7 demos cortos) | 3 días |
| T2.3 | Gamma API reference auto-generated (Doxygen) | 1 día |
| T2.4 | Tutorial: "Build a process monitor in 50 lines" | 1 día |
| T2.5 | `GAMMA_README.md` standalone (no asume Ayama) | 1 día |

**Total Tier 2**: ~1-1.5 semanas.

### §3.3 — Estructura objetivo del repo post-Tier-2

```
gma_1.0.0/
├── README.md                  # Ayama-focused, links a Gamma
├── LICENSE
├── CMakeLists.txt             # toplevel: option(GAMMA_STANDALONE)
├── ayama/                     # the app
│   └── ...
├── pillars/                   # Gamma framework
│   ├── README.md              # Gamma-focused, no asume Ayama
│   ├── examples/              # demos standalone Gamma
│   │   ├── topology_query/    # printf topology info
│   │   ├── process_monitor/   # 50-line process viewer
│   │   ├── etw_session/       # standalone ETW capture
│   │   ├── shm_pubsub/        # 2-process IPC demo
│   │   ├── working_set/       # set self memory limits
│   │   ├── wake_event/        # cross-platform wait demo
│   │   └── topology_viz/      # ASCII topology renderer
│   ├── topology/, process/, ...
│   └── _meta/                 # versioning + CMake exports
├── docs/
│   ├── ayama/                 # app docs
│   ├── gamma/                 # framework docs (auto-generated + tutorials)
│   └── figures/               # hero plots etc.
└── .github/
    └── workflows/             # CI
```

### §3.4 — CMake separation strategy

```cmake
# Toplevel CMakeLists.txt
option(GAMMA_STANDALONE "Build only Gamma framework" OFF)
option(AYAMA_BUILD "Build Ayama application" ON)

add_subdirectory(pillars)   # always build Gamma

if(AYAMA_BUILD AND NOT GAMMA_STANDALONE)
    add_subdirectory(ayama)
endif()
```

Gamma como `find_package` consumer-side:

```cmake
# Downstream project
find_package(Gamma 1.1.0 REQUIRED COMPONENTS topology process etw)
target_link_libraries(myapp PRIVATE Gamma::topology Gamma::process Gamma::etw)
```

### §3.5 — Examples — criterio de inclusión

Cada example debe ser:
- Standalone (`main.cpp` `<` 200 LOC)
- Compila contra Gamma installed sin Ayama present
- Compila en `<` 5 s
- Ejecuta en `<` 1 s y muestra output útil
- Documentado con comments tipo tutorial (no production-ready)
- Cubre **un** pilar primario (puede usar otros como soporte)

Ejemplos no canónicos como `process_monitor` deben generar valor inmediato: ver-todos-mis-procesos-con-CPU-real en `<` 50 LOC es objetivamente superior a hacerlo con raw Win32 APIs (`>` 200 LOC).

### §3.6 — Performance considerations Tier 2

- **Doxygen build**: cache config, target `<` 30 s para full re-build, `<` 5 s incremental.
- **Example compile**: cada example `<` 5 s. Si pasa, el example está demasiado complejo — partirlo.
- **Library separation cost**: `find_package` lookup `<` 100 ms. `target_link_libraries` resolution `<` 50 ms.

### §3.7 — Salida verificable

Un developer puede:
1. Clonar Gamma standalone
2. `cmake -S . -B build -DGAMMA_STANDALONE=ON`
3. `cmake --build build` (← `<` 2 min)
4. `cmake --install build --prefix /usr/local/gamma-1.1.0`
5. En su own project, `find_package(Gamma)` funciona
6. Linkea, compila, ejecuta
7. **Sin tocar Ayama code en ningún momento**

---

## §4 — Feature A: Compute Graph del agent

### §4.1 — Objetivo

Refactor `AgentRuntime::run()` para que su pipeline interno sea un **grafo explícito de operaciones con dependencias**, no una secuencia hardcoded. Esto:

- Demuestra Gamma puede infrastructure no-trivial (no solo wrappers Win32)
- Habilita visualización live del pipeline en UI (hero feature visual)
- Habilita parallelism real entre nodos sin dependencias
- Provee per-node timing automático (debugging + perf profiling gratis)

### §4.2 — Concepto

Pipeline actual del tick es:

```
metrics_sample → classify → policy_evaluate → apply_decisions → publish
                                                              → audit_drain
                                                              → self_monitor
```

Como graph explícito:

```cpp
namespace gamma::graph {
    Graph g("ayama_tick");

    auto sample    = g.add_node<MetricsSample>("metrics_sample");
    auto classify  = g.add_node<Classify>("classify").depends_on(sample);
    auto evaluate  = g.add_node<PolicyEvaluate>("policy_evaluate").depends_on(classify);
    auto apply     = g.add_node<ApplyDecisions>("apply").depends_on(evaluate);
    auto publish   = g.add_node<ShmPublish>("publish").depends_on(evaluate);
    auto audit     = g.add_node<AuditDrain>("audit_drain").depends_on(apply);
    auto monitor   = g.add_node<SelfMonitor>("self_monitor"); // no deps, parallel

    g.compile();  // topological sort, identify parallel cohorts
}

// Per-tick:
g.run();  // ~10 µs overhead vs hand-coded sequential (target: < 100 µs)
```

`publish` y `audit_drain` no comparten dependencias → corren en paralelo. `self_monitor` puede correr en paralelo con todo lo demás.

### §4.3 — Items concretos

| # | Item | Esfuerzo |
|---|---|---|
| A.1 | Design `gamma::graph` API (pillar nuevo) | 2 días |
| A.2 | Implementar `Graph`, `Node` con concepts C++23 | 4 días |
| A.3 | Topological sort + parallelism detection | 2 días |
| A.4 | Per-node TSC timing collection (lock-free) | 1 día |
| A.5 | Refactor `AgentRuntime::run()` para usar graph | 3 días |
| A.6 | UI panel: live graph visualization | 4 días |
| A.7 | SHM exposure de timing histórico per-node | 1 día |
| A.8 | Tests + benchmarks | 2 días |

**Total Feature A**: ~3 semanas.

### §4.4 — Performance restricciones críticas

**Esta feature solo se acepta si el overhead absoluto del graph runtime `<` 100 µs por tick.** El tick actual cuesta ~10 ms total; añadir más de 1% es regresión inaceptable.

Patrones obligatorios (detallados en Implementation Strategies):
- **Zero allocation en runtime**: nodes pre-allocados al `compile()`. `Graph::run()` no allocate.
- **Static polymorphism via concepts**: no `virtual` dispatch. Cada `Node` satisface concept `ExecutableNode`.
- **CRTP** alternativa si concepts no encajan estilísticamente.
- **Cache-line aligned node storage**: `alignas(64) std::array<NodeSlot, kMaxNodes>`.
- **Topological order baked at compile**: array de índices ordenado, iteración lineal.
- **Parallel cohorts pre-computed**: si nodes A, B, C son paralelos → un thread pool job dispatch única, no por-nodo.
- **Timing via `rdtsc()` directo**: `gma::hal::rdtsc()` ~10 cycles. Una llamada antes y después de cada `node.execute()`.

### §4.5 — UI Visualization

Panel nuevo: "Pipeline" (tab adicional o reemplaza tab existente).

Render:
- Nodes como rectángulos con `ImDrawList::AddRectFilled`
- Edges como bezier curves con `ImDrawList::AddBezierCubic`
- Color de cada node according to timing: verde si `<` 1ms, amarillo `<` 5ms, rojo `>` 5ms
- Hover: tooltip con sparkline de history (últimos 60 ticks)
- Layout: depth = topological depth, horizontal spread = parallel cohort siblings

Costo render: `<` 1ms/frame al 60 FPS.

### §4.6 — Salida verificable

Un visitante del repo abre la UI y ve:
- Tab "Pipeline" con el grafo del agent en tiempo real
- Cada node mostrando su timing actual
- Edges con flujo visual
- Identifica visualmente qué fases son paralelas

Esto es **el screenshot de marketing #1**.

---

## §5 — Feature B: Topology Map UI

### §5.1 — Objetivo

Renderizar el CPU topology como mapa visual interactivo, con threads de cada proceso mostrados live sobre los cores donde corren. Esta es la **otra hero visualization** que documenta lo que Gamma sabe sobre el hardware.

### §5.2 — Concepto

Para 7950X3D:
```
┌──────────── CCD0 (V-Cache, 96 MB L3) ────────────┐
│ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐
│ │ 0  │ │ 1  │ │ 2  │ │ 3  │ │ 4  │ │ 5  │ │ 6  │ │ 7  │
│ │ 87%│ │ 12%│ │ 5% │ │ 0% │ │ 1% │ │ 0% │ │ 0% │ │ 0% │
│ │MCC │ │MCC │ │... │ │... │ │... │ │... │ │... │ │... │
│ └────┘ └────┘ └────┘ └────┘ └────┘ └────┘ └────┘ └────┘
└──────────────────────────────────────────────────┘
┌──────────── CCD1 (Standard, 32 MB L3) ───────────┐
│ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐
│ │ 8  │ │ 9  │ │ 10 │ │ 11 │ │ 12 │ │ 13 │ │ 14 │ │ 15 │
│ │ 0% │ │ 8% │ │ 0% │ │12% │ │ 0% │ │ 0% │ │ 2% │ │ 0% │
│ │... │ │Discord│... │ │Chrome│... │ │... │ │ay-ag│... │
│ └────┘ └────┘ └────┘ └────┘ └────┘ └────┘ └────┘ └────┘
└──────────────────────────────────────────────────┘
```

Cada core:
- CPU% live (color heatmap)
- Process name dominante (con threads en ese core)
- Tooltip con detalle al hover
- Visualización clara de qué CCD tiene V-Cache

### §5.3 — Items concretos

| # | Item | Esfuerzo |
|---|---|---|
| B.1 | Extender `gma::proc::ProcessMetricsSnapshot` para per-thread ideal_processor info | 1 día |
| B.2 | Per-core CPU% sampling (Gamma extension) | 2 días |
| B.3 | Per-core history ring buffer (60 samples × 16 cores) | 1 día |
| B.4 | TopologyMap panel con ImGui custom drawing | 3 días |
| B.5 | Hover tooltips con detalle de processes | 1 día |
| B.6 | History sparklines per core | 1 día |
| B.7 | V-Cache indicator visual | 0.5 día |
| B.8 | Tests + responsive layout | 1 día |

**Total Feature B**: ~1-1.5 semanas.

### §5.4 — Performance restricciones

- **Sampling rate**: per-core CPU% sampleado cada 100ms (10 Hz). Suficiente para perception, no inunda CPU.
- **Memory**: 16 cores × 60 samples × 4 bytes float = 3.75 KB ring buffer. Trivial.
- **Render**: panel a 60 FPS `<` 0.5ms cost. Direct `ImDrawList` primitives, no widgets pesados.
- **Per-thread → per-core resolution**: lectura de `SYSTEM_THREAD_INFORMATION::ThreadState` de la captura NtQSI que ya hacemos. Cero syscalls adicionales.

### §5.5 — Salida verificable

UI panel:
- Muestra topology fiel al hardware actual (probe vía `gma::hw::topology()`)
- CPU% live por core, refrescando 10 Hz
- Process names visibles en cores activos
- V-Cache CCD claramente diferenciado visualmente
- Funcional en 7950X3D (validado), 7800X3D (validar), Intel hybrid (validar)

Esto es **screenshot de marketing #2** + es **producto real** de toda la información que Gamma agrega.

---

## §6 — Secuencia y dependencias

### §6.1 — DAG de dependencias

```
T0 (repo hygiene) ──┬─→ T1 (empirical evidence)
                    │
                    └─→ T2 (Gamma standalone)
                              │
                              └─→ A (Compute Graph) ─┐
                              │                       ├─→ Release
                              └─→ B (Topology Map) ──┘
```

- T0 es prerequisito absoluto (sin repo limpio, todo lo demás se ve mal).
- T1 y T2 son independientes (pueden paralelizarse si hay dos contributors).
- A y B requieren T2 (Compute Graph usa `gamma::graph` pillar — necesita la separación standalone para tener sentido).
- A y B son independientes entre sí.

### §6.2 — Timeline propuesto (single contributor)

| Semana | Items |
|---|---|
| 1 | T0 (repo hygiene) — todo |
| 2 | T1 + T2.1 (CMake split) en paralelo de mañana/tarde |
| 3 | T2.2-T2.5 (examples, docs, README Gamma) |
| 4-6 | Feature A (Compute Graph) |
| 7 | Feature B (Topology Map) |
| 8 | Polish, bug bash, release prep |

**Total: 8 semanas de trabajo full-time enfocado** para alcanzar showcase release v1.0.

### §6.3 — Release intermedios

No releasear hasta v1.0 completo es contraproducente. Releases intermedios para feedback:

| Release | Items incluidos | Audiencia |
|---|---|---|
| **v0.5** (post-T0) | repo + binaries, NO examples | Early developers (you+circle) |
| **v0.7** (post-T1+T2) | + empirical evidence + Gamma docs | Beta testers |
| **v0.9** (post-A) | + Compute Graph viz | Marketing-ready preview |
| **v1.0** (post-B) | + Topology Map | Public release |

Cada release intermedio = chance de feedback antes de v1.0. Bug reports tempranos `<<` 100x cheaper que post-public.

---

## §7 — Deliverables y criterios de éxito

### §7.1 — Tier 0 deliverables

- [ ] Repo público en GitHub bajo Apache 2.0
- [ ] README.md visible con hero plot
- [ ] CI verde en ambas matrices Windows (MSVC + MinGW)
- [ ] Release zip v0.5 publicado en GitHub Releases
- [ ] Install script ejecutable en fresh Windows 11

**Criterio de éxito**: developer externo clone → ejecuta → reporta UI funcional en `<` 5 min total.

### §7.2 — Tier 1 deliverables

- [ ] `EMPIRICAL_EVIDENCE_SUMMARY.md` polish público (English completo)
- [ ] Hero plot PNG checkeado en `docs/figures/halo-mcc-curve.png`
- [ ] `REPRODUCING_BENCHMARKS.md` con paso a paso
- [ ] Reports individuales con formato consistente

**Criterio de éxito**: un investigador puede replicar un test del catálogo sin contactar al autor.

### §7.3 — Tier 2 deliverables

- [ ] `pillars/CMakeLists.txt` con install target
- [ ] `examples/` con 7 demos compilables independientes
- [ ] Doxygen API reference generada y servida (GitHub Pages?)
- [ ] `GAMMA_README.md` con tutorial 50-line
- [ ] `find_package(Gamma 1.1.0)` funciona en downstream project de prueba

**Criterio de éxito**: developer crea project nuevo, linkea a Gamma instalado, ejecuta su own demo en `<` 30 min.

### §7.4 — Feature A deliverables

- [ ] `pillars/graph/` pillar nuevo con API documentada
- [ ] `AgentRuntime::run()` refactored para usar graph
- [ ] UI panel "Pipeline" funcional
- [ ] Benchmark: overhead `<` 100 µs/tick validado
- [ ] Tests unitarios del graph pillar (n ≥ 10)

**Criterio de éxito**: screenshot del panel "Pipeline" mostrando `<` 5ms por nodo en condiciones normales, con paralelismo visible.

### §7.5 — Feature B deliverables

- [ ] TopologyMap panel funcional
- [ ] Validado en 7950X3D, 7800X3D, Intel hybrid (al menos 1 de cada)
- [ ] Per-core CPU% accuracy `±5%` vs Process Explorer
- [ ] Refresh visible at 10 Hz sin flicker

**Criterio de éxito**: screenshot del panel con el hardware del visitante mostrado fielmente.

---

## §8 — Out-of-scope (features deferred)

Items mencionados en discussion previa pero **NO** en este plan:

### §8.1 — Feature C: Comparative Benchmark Framework

Defer porque requiere instalar/detectar Process Lasso, cuya licencia es propietaria. No legal/ético bundlearlo. Si se hace, debe ser opcional add-on que el user instala separadamente.

**Reschedule**: post v1.0, si comunidad pide explícitamente.

### §8.2 — Feature D: Plugin Architecture

Defer porque ABI design es delicado y crystallizar APIs antes de v1.0 mata flexibility downstream. Hacer plugins requiere haber probado Gamma APIs en N projects diferentes — todavía no estamos ahí.

**Reschedule**: v2.x, después de feedback de usuarios reales de Gamma standalone.

### §8.3 — Feature E: Config DSL (TOML rules)

Defer porque audience de "power users que escriben rules custom" es < 5% de usuarios. v1.0 con defaults sólidos resuelve 95% de casos.

**Reschedule**: si llegamos a v1.5+, considerar como extensibilidad opcional.

### §8.4 — Linux port real de Gamma

Mantener Linux paths como stubs hasta haya demanda real. Un pillar standalone Linux (e.g. `topology`) puede hacerse como demo de cross-platform en v1.1.

### §8.5 — Anti-cheat Safe Mode

Out of scope hasta v2.x. Requiere investigación legal + protocol con vendors anti-cheat (Vanguard, EAC). No alcanzable solo.

### §8.6 — Public release readiness / installer signing

Code signing certificate, installer (NSIS/MSIX), Windows Defender SmartScreen whitelist — todo defer hasta v1.0 valide tracción mínima (§0.3).

---

## §9 — Tracking y governance

### §9.1 — Cómo trackear progreso

- Cada item con prefijo T0./T1./T2./A./B. es un GitHub issue post-T0.
- Milestones: v0.5, v0.7, v0.9, v1.0.
- Project board público en GitHub: To Do / In Progress / Done.
- Commits referencian issue: `feat(graph): topological sort #42`.

### §9.2 — Reglas de aceptación de PRs externos

(Aplica solo post-T0 cuando el repo es público)

- Toda PR a master debe ser ≤ 500 LOC neto (refactor mayor → discutir en issue primero).
- Toda PR debe pasar CI en ambas matrices.
- PRs que toquen Gamma deben venir con test (cobertura no negociable).
- PRs que toquen Ayama policies deben venir con report empírico A/B/A/B/A o justificación de por qué no aplica.

### §9.3 — Versioning

Semantic versioning estricto post v1.0:

- **Major (X.y.z)**: ABI break en Gamma o IPC layout change
- **Minor (x.Y.z)**: nuevo feature backward compatible
- **Patch (x.y.Z)**: bug fix sin nuevas APIs

Pre-v1.0 (v0.5 → v0.9): cualquier cosa puede romperse. Documentar en changelog.

---

*Plan aprobado para implementación tras finalización Phase B.2 (mayo 2026). Re-evaluar después de v0.5 si tracción justifica continuar a v1.0 o pivot a paper académico únicamente.*
