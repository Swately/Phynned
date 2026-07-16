> **📦 Archived planning document — historical.**
> From Phyriad's pre-rebrand era (the project was codenamed *gamma* / *gma*).
> Published for transparency about how the project was actually built — **not**
> a current specification. Version numbers and performance claims here
> (including any "vs Folly" / "production-ready" framing) reflect the project's
> state at the time of writing and may since have been revised or **retracted**;
> the project has reset to `v0.1.0-experimental`. For current status see the
> repo `README.md` and `docs/planning/`. For benchmark-claim validation status
> see `docs/planning/TEST_INVENTORY.md`.

# Ayama — Refinement Master Plan (Phases A / B / C)

*Path to maximum CPU + GPU performance leverage on AMD asymmetric V-Cache
CPUs, executed in clean Gamma-integrated layers.*

**Versión:** 1.0 — 2026-05-17 (post-empirical-validation phase).
**Status:** Planning. No code written from this plan yet.
**Pre-requisitos:** 9 empirical reports + `EMPIRICAL_EVIDENCE_SUMMARY.md` validados.

---

## §0 — Objetivos y no-objetivos

### Objetivo principal

Empujar el catálogo empírico de **9 → 15+ tests validados** con verdict
distribution que confirme:

| Tier | Hoy | Después de Phase B |
|---|---|---|
| SIGNIFICANT | 2 (Halo 2, Fallout 4) | 3-5 (más legacy + UE5 si differential pinning funciona) |
| MARGINAL+ | 2 (Hogwarts, BL2 high-FPS) | 4-6 |
| NULL (architecturally explained) | 4 | Retain or upgrade |

### Filosofía

1. **Refinar lo que tenemos**, no construir desde cero.
2. **Cero código transitorio** que dependa de hacks Win32 cuando podríamos
   usar APIs limpias de Gamma. Si Gamma carece de algo → filar Feature
   Request antes de codear el workaround en Ayama.
3. **Cada fase se valida empíricamente** antes de pasar a la siguiente.
   Si Phase B no muestra delta, no escalamos a Phase C.

### Lo que NO buscamos en este plan

- **NO completar el Vulkan stub de Gamma.** Ortogonal — Vulkan stub es
  para *renderizado*; Ayama-GPU es para *optimización* de threads
  GPU-adjacentes. No comparten código.
- **NO Safe Mode anti-cheat compatibility.** Diferido — requeriría
  refactor del classifier + feature flags + auditoría de seguridad.
- **NO hardware coverage expansion** (Intel hybrid, 7800X3D single-CCD).
  Validar primero la palanca máxima en 7950X3D antes de generalizar.

---

## §1 — Phase A: Tooling completion (~3-4h, no Gamma FRs nuevos)

Quick wins de tooling que **cierran el flujo empírico end-to-end**. Cero
deuda de Gamma, no bloquean Phase B.

### A.1 — Max-frame-time as verdict driver
**Archivo:** `ayama/tools/ayama-cli/main.cpp` `cmd_bench_multi`
**Esfuerzo:** ~45 min
**Gamma FR:** ninguno
**Justificación:** BL2 high-FPS demostró que `bench multi` puede emitir
NULL cuando P99 es SIGNIFICANT y max-frame-time se redujo -50% — el
verdict tool no captura outlier elimination. Fix: añadir max_ms al
modelo de signals + criteria del verdict.

**Cambios concretos:**
1. Extender `GroupStat` con `max_ms_mean`, `max_ms_stddev`.
2. Parsear `max_ms` del bench.csv (campo ya existe).
3. Añadir tabla de max_ms al output.
4. Actualizar verdict logic: `MARGINAL+` si `(P99 SIG || max_ms SIG)` y P99.9 trending.

**Rollback:** archivo único, cambio aditivo, fácil revertir.

### A.2 — Report exporter automático desde UI
**Archivos:** `ayama/tools/ayama-ui/BenchRunner.hpp` + `widgets/bench_panel.hpp`
**Esfuerzo:** ~1.5h
**Gamma FR:** ninguno
**Justificación:** Cierra el "PowerShell-zero workflow" prometido en
Track 2. Hoy los reports se escriben a mano (`.md`); deberían generarse
por click.

**Cambios concretos:**
1. Añadir botón "Export report" en bench panel (solo visible cuando state == Done).
2. Template `.md` generator: toma BenchRun[] + aggregate_log, escribe `report_<game>_<date>.md` siguiendo el formato de los reports existentes.
3. File picker para destino (Win32 `IFileDialog`).
4. Helper `generate_md_report(runs, aggregate, output_path)`.

**Rollback:** botón opcional; no afecta workflow existente.

### A.3 — ImPlot integration (con decisión Gamma-first)
**Esfuerzo:** ~4-6h
**Gamma FR:** **DECISIÓN ARQUITECTÓNICA** antes de codear (ver §4)
**Justificación:** Charts visuales en tiempo real de frame-time durante
captura — UX polish, no funcional. Permitiría que el usuario VEA spikes
mientras corren los benchmarks.

**Decisión arquitectónica antes de codear**:
- **Opción 1**: añadir ImPlot al pillar `gamma_ui` de Gamma → todos los
  proyectos Gamma-based lo tendrían disponible (incluye Ayama).
- **Opción 2**: añadir ImPlot solo al target `ayama-ui` en
  `ayama/tools/ayama-ui/CMakeLists.txt` → código transitorio en Ayama,
  futuros proyectos Gamma lo replicarían.

**Recomendación**: Opción 1 (Gamma-side). Justificación: ImPlot es la
extensión canonical de ImGui para charting, encaja con el design pattern
"Gamma maneja el render stack, Ayama solo se preocupa por contenido".

**Gamma FR a filar antes de codear Ayama-side:**
> **GFR-Ayama-1**: Add ImPlot to `gamma_ui` pillar
> - Add ImPlot source as CMake FetchContent or vendored
> - Expose `imgui_implot` target as transitive dep of `gamma_ui`
> - Validates with a render test (sample plot in `examples/standard_window`)

**Rollback:** si Gamma rechaza GFR-Ayama-1, fallback a Opción 2.

### A.4 — Dev/User view toggle
**Archivos:** `ayama/tools/ayama-ui/main.cpp` + state management
**Esfuerzo:** ~3-4h
**Gamma FR:** ninguno
**Justificación:** Tu pregunta original de la sesión — vista
simplificada para usuario final, vista completa para developer/empirical
testing. Cleaner cuando enseñes Ayama a alguien no-técnico.

**Cambios concretos:**
1. Top menubar: `View → [Developer | End-user]`
2. AyamaAppState gana flag `view_mode`
3. Dashboard panel: refactorizar para mostrar subset minimal en User mode (semáforo de estado, único botón "Run benchmark", lenguaje plano)
4. Tabs Targets / Policies / Actions / Advanced: ocultos en User mode
5. Persistencia simple del modo via `LOCALAPPDATA\Ayama\ui.toml`

**Rollback:** dev mode = default; user mode añade overlay.

---

## §2 — Phase B: CPU-side power-ups (~8-12h, requiere Gamma FRs)

**Aquí es donde se gana lo grande.** Los items de Phase B atacan los
NULLs actuales del catálogo (RDR2, Minecraft modded, BL2 normal-FPS) y
los modernizan a MARGINAL/SIGNIFICANT.

### B.1 — Differential thread pinning (el item más ambicioso)
**Esfuerzo:** ~6-8h Ayama-side + 2 Gamma FRs
**Magnitud esperada:** +5-15% en games modernos que hoy son NULL
**Riesgo:** Alto — toca el hot path del PolicyEngine

**Lo que hace:** En lugar de pinear TODO el proceso del game a un CCD,
identifica threads por rol:
- **Main game thread** (más CPU% sostenido) → V-Cache CCD (CCD0)
- **Worker pool threads** (varios threads con CPU% medio) → non-V-Cache CCD (CCD1)
- **GPU command-recorder threads** (correlación con DXGI events) → V-Cache CCD junto al main
- **I/O threads** (low CPU, blocked frequently) → cualquier core, deja al scheduler

Esto **distribuye L3 working set** entre los dos CCDs en vez de
forzar todo en uno. Para engines bien-threaded (UE5, RAGE, modded Java)
podría desbloquear el caso donde Ayama hoy no ayuda.

**Cambios Ayama-side:**
1. `MetricsCollector`: extender de per-process a per-thread sampling
2. `ProcessClassifier`: añadir thread role classifier (peak CPU% rolling avg, ETW correlation)
3. `PolicyEngine`: nuevas reglas `PinMainThreadToVCache`, `EvictWorkersFromVCache`
4. `ActionExecutor`: aplicar via `set_thread_affinity` por TID

**Gamma FRs requeridas antes de codear Ayama-side:**

> **GFR-Ayama-2**: `gma::hw::set_thread_affinity(uint32_t tid, uint64_t mask)`
> - Symmetric a FR-3 (`set_process_affinity`) pero a nivel thread
> - Windows: `OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION) + SetThreadAffinityMask`
> - POSIX: `pthread_setaffinity_np` o `sched_setaffinity(tid, ...)`
> - Returns previous mask for revert
> - `noexcept`, `std::expected<uint64_t, gma::Error>`

> **GFR-Ayama-3**: `gma::proc::enumerate_threads(uint32_t pid, ThreadEntry* out, uint32_t max_count)`
> - Bulk thread enumeration similar a FR-4 (`enumerate_processes`)
> - Windows: `NtQuerySystemInformation` con SystemProcessInformation incluye thread arrays
> - POSIX: `/proc/<pid>/task/*`
> - Returns ThreadEntry { tid, kernel_time_100ns, user_time_100ns, state }
> - Zero heap allocation

**Validación:** test inicial con RDR2 / Minecraft modded para confirmar
que el NULL se convierte en MARGINAL+ antes de declarar B.1 done.

### B.2 — Memory NUMA awareness
**Esfuerzo:** ~3-4h Ayama-side + 1 Gamma FR
**Magnitud esperada:** +2-5% adicional, complementa V-Cache pin
**Riesgo:** Bajo

**Lo que hace:** El 7950X3D tiene **dos NUMA nodes** (uno por CCD). La
memoria asignada por el game puede estar en el NUMA node "equivocado"
respecto al CCD donde se pinea. Ayama puede sugerir al kernel que
asocie ideal processor + memory hint al thread principal.

**Cambios Ayama-side:**
1. `PolicyEngine`: extender PolicyDecision con `ideal_processor` field
2. `ActionExecutor`: nuevo `apply_set_thread_ideal_processor`
3. PolicyDecisions del PinGameToVCacheCcd ahora setean `ideal_processor = first_vcache_core`

**Gamma FR requerida:**

> **GFR-Ayama-4**: `gma::hw::set_thread_ideal_processor(uint32_t tid, uint32_t logical_id)`
> - Windows: `SetThreadIdealProcessor` (deprecated alias of `SetThreadIdealProcessorEx`)
> - POSIX: portable subset — `pthread_setaffinity_np` con single-CPU mask
> - Less strict que set_thread_affinity — sugiere al scheduler, no fuerza
> - Returns previous ideal processor

**Por qué es complementario a B.1:** affinity es restrictivo (thread NO
puede correr en otros cores). Ideal processor es sugerencia (scheduler
prefiere ese core pero puede mover el thread si está saturado). Usar
ambos da el placement deseado + flexibility.

### B.3 — Cross-process working set tuning
**Esfuerzo:** ~2h Ayama-side + 1 Gamma FR
**Magnitud esperada:** +0-3% (low magnitude, free win)
**Riesgo:** Muy bajo

**Lo que hace:** Forzar working-set min/max para el game process. Evita
paginación durante streaming events.

**Gamma FR requerida:**

> **GFR-Ayama-5**: `gma::tuning::set_process_working_set(uint32_t pid, uint64_t min_bytes, uint64_t max_bytes)`
> - Cross-process variant of existing FR-19 `set_self_working_set`
> - Windows: `OpenProcess(PROCESS_SET_QUOTA) + SetProcessWorkingSetSizeEx`
> - POSIX: `/proc/<pid>/oom_score_adj` + `madvise`-style hints (limited)

---

## §3 — Phase C: GPU complement (opcional, ~10-15h)

Solo si Phase B no agota el roadmap. Magnitud esperada **menor** que
Phase B (+3-10% en games D3D12 CPU-bound) pero más esfuerzo de
ingeniería.

### C.1 — GPU observer (PDH-based)
**Esfuerzo:** ~3h
**Gamma FR:** ninguno — usamos PDH directamente
**Magnitud:** 0% (sólo observación, no boost)
**Justificación:** Telemetría per-process. Permite filtrar tests "GPU-bound
genuino" vs "CPU-bound con hidden bottleneck".

### C.2 — ETW DxgKrnl integration
**Esfuerzo:** ~3h Ayama-side + 1 Gamma FR
**Gamma FR:** GFR-Ayama-6 (DxgKrnl provider GUID + parser)
**Justificación:** Identifica qué threads del game hacen calls D3D12.

> **GFR-Ayama-6**: `gma::etw::providers::kDxgKrnl` + event parsers
> - Add provider GUID for `Microsoft-Windows-DxgKrnl`
> - Add event parsers for `Present`, `QueueSubmit`, `Vsync`
> - Headers-only, no new pillar

### C.3 — Command-thread pinning
**Esfuerzo:** ~4h
**Dependencias:** B.1 (thread affinity) + C.2 (DxgKrnl events)
**Magnitud:** +3-10% en D3D12 CPU-bound games

Identifica threads que disparan `DxgKrnl::Present` via ETW correlation,
pinealos al V-Cache CCD junto al main thread.

---

## §4 — Gamma Feature Requests requeridas (sumario)

Antes de codear Phase B necesitamos:

| FR ID | Title | Phase blocked | Priority |
|---|---|---|---|
| **GFR-Ayama-1** | ImPlot in gamma_ui | A.3 | Medium |
| **GFR-Ayama-2** | `set_thread_affinity` | **B.1 (the big one)** | **HIGH** |
| **GFR-Ayama-3** | `enumerate_threads` | **B.1** | **HIGH** |
| **GFR-Ayama-4** | `set_thread_ideal_processor` | B.2 | Medium |
| **GFR-Ayama-5** | `set_process_working_set` | B.3 | Low |
| **GFR-Ayama-6** | `kDxgKrnl` ETW provider | C.2 | Low (Phase C only) |

**Process:**
1. Filar las 6 FRs en `docs/ayama/GAMMA_FEATURE_REQUESTS.md` con detalle
2. Gamma team (o quien gestione Gamma) implementa
3. Ayama side espera APIs disponibles antes de codear

**Mientras Gamma implementa, Ayama-side procede con Phase A** (no
bloqueado por FRs).

---

## §5 — Sequencing recommendation

**Sesión 1 (~3-4h)**: Phase A items A.1, A.2 (tooling, sin Gamma deps)
- Item A.1 max-frame-time verdict — ~45 min
- Item A.2 Report exporter — ~1.5h
- Buffer + testing — ~1h

**Sesión 2 (~3-4h)**: Phase A items A.3, A.4 (depending on Gamma decision)
- Decision: ImPlot Gamma-side vs Ayama-side
- If Gamma-side: file GFR-Ayama-1, wait
- If Ayama-side: implement A.3 directly
- Implement A.4 (Dev/User view)

**Sesión 3 (~1h)**: File Gamma FRs (B.1-B.3 + C.2 stack)
- Write detailed FRs in GAMMA_FEATURE_REQUESTS.md
- Mark dependencies clearly
- Hand to Gamma team

**Sesión 4+ (~8-12h, after Gamma FRs implemented)**: Phase B
- B.1 differential thread pinning (the headline feature)
- B.2 NUMA awareness
- B.3 working set tuning
- Validation tests (RDR2, Minecraft modded re-test) — these were NULL before, target MARGINAL+ after

**Sesión X+ (opcional)**: Phase C if Phase B validates the model

---

## §6 — Risk register

| Risk | Mitigation |
|---|---|
| GFR-Ayama-2/3 take longer than expected from Gamma side | Phase A keeps us productive in parallel |
| B.1 differential pinning regresses some games that today are SIGNIFICANT (Fallout 4, Halo 2) | A/B/A/B/A test ALL games in catalog post-B.1 before declaring done |
| Phase C GPU complement has lower-than-expected leverage | Optional phase, can skip if Phase B already maximizes |
| ImPlot integration adds dependency bloat to gamma_ui | Decision is reversible (Opción 2 fallback to Ayama-side) |
| Thread-affinity for game's main thread fights with engine's own thread affinity calls | Detect engine-set affinity, respect it (don't override if game explicitly pinned itself) |

---

## §7 — Definition of Done per Phase

### Phase A done when:
- [ ] A.1: BL2 high-FPS test verdict shows MARGINAL+ (instead of NULL) due to max-frame-time signal
- [ ] A.2: Report exporter generates valid .md matching report format with one click
- [ ] A.3: ImPlot charts visible during bench captures (decision Gamma vs Ayama side documented)
- [ ] A.4: Dev/User toggle works, User mode hides advanced tabs, persists across launches

### Phase B done when:
- [ ] B.1: All 6 Gamma FRs (GFR-Ayama-2 through -5) implemented and validated
- [ ] B.1: Differential pinning policy works on Hogwarts test (existing MARGINAL maintained or improved)
- [ ] B.1: RDR2 or Minecraft modded re-test moves from NULL to MARGINAL+
- [ ] B.2: NUMA hint applied; test repeat of Halo 2 / Fallout 4 maintains SIGNIFICANT
- [ ] B.3: Working-set tuning applied; no regression on existing tests
- [ ] Full catalog re-tested: no SIGNIFICANT regressed to NULL

### Phase C done when:
- [ ] C.1: GPU observer per-process working
- [ ] C.2: GFR-Ayama-6 implemented; DxgKrnl events received
- [ ] C.3: D3D12 CPU-bound games show additional +3% delta over Phase B

---

## §8 — Open questions for the user

Before kicking off Phase A:

1. **ImPlot integration**: Gamma-side (file GFR-Ayama-1) or Ayama-side
   (skip the FR)? Recommendation: Gamma-side.

2. **Phase C scope**: do we commit to it now, or decide after Phase B
   results? Recommendation: defer decision to post-Phase-B.

3. **Test catalog expansion**: should we test Halo CE Anniversary,
   Skyrim SE, etc. AFTER Phase A but BEFORE Phase B? They would expand
   the empirical baseline against which Phase B improvements are
   measured.

---

*Fin del Refinement Master Plan v1.0.*
*Acoplado a:* `AYAMA_MASTER_PLAN.md` (project-wide),
`EMPIRICAL_TEST_PROTOCOL.md` (testing methodology),
`EMPIRICAL_EVIDENCE_SUMMARY.md` (validation evidence base),
`GAMMA_FEATURE_REQUESTS.md` (Gamma-side dependencies).
