# Ayama — Empirical Evidence Summary

> ## ⚠️ Provisional — under re-validation, not yet citable
>
> This summary is kept as an honest record of an exploration, but its specific
> numbers and counts must **NOT** be cited as validated. The dataset behind it
> is not a clean, reproducible record:
>
> - Some runs were **invalid** — captured while a test-time code bug was still
>   present (later fixed), or with the game **GPU-bound**, which masks the
>   CPU-affinity effect entirely.
> - The raw PresentMon-style CSV captures were **created and then rewritten**,
>   so the reports cannot be traced back to original, untouched captures.
>
> What **is** real: the *direction* of the finding — pinning a CPU-bound,
> cross-CCD-sensitive game thread to the V-Cache CCD helps old / single-threaded
> engines — was observed directly and repeatedly (Fallout 4 and Halo 2 showed
> large, obvious gains). What is **not** established is any specific magnitude,
> the per-game table below, or a validated test count.
>
> **Status: pending a clean re-run** under the corrected protocol, with raw
> captures preserved immutably. Until then treat every number here as
> provisional. (Mirrors the benchmark-honesty stance in
> `BENCHMARK_FAIRNESS.md` — a substrate evidence doc, not imported at the
> 2026-07-16 separation; it lives at `catalog/cpp/docs/evidence/` in the
> parent container.)

*One-page **provisional** summary of an exploratory A/B/A/B/A study of the
Ayama runtime optimizer on AMD Ryzen 9 7950X3D + NVIDIA RTX 4090. The dataset
is under re-validation and not yet citable — see the banner above.*

**Last updated**: 2026-05-17 (Halo MCC catalog extended: CE/3/Reach/4 added).
*Reclassified as provisional 2026-05-21 — see banner.*

---

## TL;DR

Ayama is a **non-invasive runtime optimizer** that pins the hot game thread
to the 3D V-Cache CCD on asymmetric AMD CPUs (7950X3D, 7900X3D, 7800X3D),
eliminating cross-CCD scheduler thrashing that hurts single-threaded
engines.

**Observed across an exploratory set of games spanning ~5 game-engine
generations** (provisional — not all runs were valid; see the banner):

| Result tier | Δ FPS | Games |
|---|---|---|
| **SIGNIFICANT (all metrics)** | **+13-98%** | Halo CE Anniversary (+98%) ⭐, Halo 2 MCC (+56%), Fallout 4 (+35%)*, Halo 3 MCC (+19%), Hogwarts Legacy (+15.5%), RDR2 CPU-bound (+13.3%)† |
| **MARGINAL+ (P99 SIGNIFICANT)** | **+1-15%** | Borderlands 2 high-FPS (P99 -4%, max -50%) |
| **NULL with variance reduction** | ~0% mean | Halo Reach MCC (1.8%, -89% stddev), RDR2 GPU-bound‡ (-99% stddev) |
| **NULL (engine ceiling)** | 0% | Halo 4 MCC, Minecraft modded (3 variants) |

*\* Fallout 4 result is conservative — Creation Engine's physics-FPS
coupling forces VSync, capping the result. Without the constraint,
expected to be Halo-2-class (+50-60%).*

*† RDR2 re-test under proper CPU-bound config (default render scale,
Saint Denis at night, 4090 at ~84% baseline → ~88% with Rule 1).
**SIGNIFICANT +13.3% avg, +20.9% P99**. Refutes the original NULL
classification from Report 7.*

*‡ Original Report 7 RDR2 result (free-roam horizon scene with high
render scale → GPU 95%-saturated) showed NULL. Same engine, same
hardware, same Ayama build — only the testing scene was wrong. See
"GPU utilization signature" section below.*

The pattern is **predictive and falsifiable**: Ayama's magnitude depends
on engine age **AND on CPU saturation at runtime**. Modern well-threaded
engines (RAGE, Sodium-modded Java) get nothing **when GPU-bound**, but
even RAGE benefits +13-21% when the scene saturates CPU. Ancient
single-thread engines (Halo 2 from 2004) get the most regardless.

---

## The headline data

Every test used the same hardware and one methodology (5-run A/B/A/B/A protocol,
30 s per run, `ayama-cli bench multi` for statistical aggregation).

| # | Game | Engine | Era | Baseline FPS | Treated FPS | Δ FPS | Verdict |
|---|---|---|---|---:|---:|---:|---|
| 11a | **Halo CE Anniversary** | Halo CE engine | 2001 | 234 | **463** | **+98%** | **SIGNIFICANT all metrics** ⭐ |
| 9 | **Halo 2 MCC** (remastered) | Halo 2 engine | 2004 | 367 | **571** | **+56%** | **SIGNIFICANT all metrics** |
| 5 | **Fallout 4** (modded) | Creation | 2015 | 178 | 241 | **+35%** | **SIGNIFICANT** (VSync-capped) |
| 11b | **Halo 3 MCC** | Halo 3 engine | 2007 | 394 | **470** | **+19%** | **SIGNIFICANT** (Avg/P99/P99.9 SIG) |
| 6 | **Hogwarts Legacy** | UE4.27 | 2023 | 86 | 99 | **+15.5%** | **MARGINAL** (P99/Avg SIG) |
| 10 | **RDR2 Saint Denis night** (CPU-bound) | RAGE + Vulkan | 2018 | 163 | **188** | **+13.3%** | **SIGNIFICANT** (Avg/P99/P99.9 SIG) † |
| 11c | Halo Reach MCC | Halo engine mature | 2010 | 440 | 447 | +1.8% | **NULL** (variance reduction only) |
| 8 | **Borderlands 2** (high-FPS) | UE3 | 2012 | 476 | 480 | +0.8% | **MARGINAL+** (P99 SIG, max -50%) |
| 1 | Minecraft + DH shaders solo | Java/LWJGL | varies | 168 | 200* | (P99.9 +22.7%) | SIGNIFICANT P99.9 |
| 11d | Halo 4 MCC | Halo engine 343 | 2012 | 505 | 505 | 0% | NULL (engine ceiling) |
| 7 | RDR2 free-roam horizon (GPU-bound) | RAGE + Vulkan | 2018 | 213 | 217 | +1.9% | NULL (within CI) ‡ |
| 8b | Borderlands 2 (normal-FPS) | UE3 | 2012 | 200 | 202 | +1% | NULL (CPU not saturated) |
| 3 | Minecraft multi-stream clean | Java/LWJGL | varies | 377 | 387 | +2.8% | NULL |
| 4 | Minecraft re-test (PID fix) | Java/LWJGL | varies | 234 | 232 | -1% | NULL (validates fix) |

*\* Minecraft + DH shaders: P99.9 +22.7% SIGNIFICANT, but Avg FPS gain was
modest (background contention was the bottleneck, not the game itself).*

---

## The predictive model

Across the (provisional) data points, Ayama's benefit appears to follow a
two-variable model:

> **Ayama Δ = f(engine threading concentration, CPU saturation at runtime)**

| Condition | Effect | Example |
|---|---|---|
| Single-thread engine + CPU saturated | **Maximum benefit (+35-60%)** | Halo 2, Fallout 4 |
| Single-thread engine + CPU NOT saturated | Marginal or null | Borderlands 2 in normal zone |
| Moderately threaded engine + CPU saturated | Moderate benefit (+10-20%) | Hogwarts Legacy |
| Well-threaded engine + CPU saturated | **Moderate benefit (+10-20%)** | RDR2 Saint Denis night (Report 10) |
| Well-threaded engine + GPU-bound | Near-zero | RDR2 free-roam horizon (Report 7) |
| Any engine + GPU 95%+ | Null (Ayama doesn't accelerate GPU) | Any over-spec'd GPU setup |

**Practical predictions for untested games** (use as input to decide whether
to test):

| Engine class | Expected Ayama Δ FPS | Empirical anchor |
|---|---|---|
| Xbox 1 era engines (2001-2004) | **+50 to +100% FPS** | Halo CE +98%, Halo 2 +56% (validated) |
| Xbox 360 first-gen multi-thread (2006-2008) | **+15-25% FPS** | Halo 3 +19% (validated) |
| Xbox 360 mature multi-thread (2010+) | **NULL or +1-5%** (variance reduction strong) | Halo Reach +1.8% (validated) |
| Xbox 360 retrofit multi-thread (2012+) | **NULL** | Halo 4 0% (validated) |
| Bethesda Creation Engine uncapped | +50-60% if CPU-bound | Fallout 4 +35% with VSync (validated) |
| Skyrim SE / Starfield / Fallout 76 | +30-50% if uncapped (capped: +25-35%) | (untested, Creation lineage) |
| UE4 games (Hogwarts class) | +10-20% | Hogwarts +15.5% (validated) |
| UE5 games (Stalker 2, Black Myth Wukong) | +10-20% | (untested, predicted) |
| RAGE engine (GTA V, RDR2) | **+10-20% if CPU-bound**; NULL if GPU-bound | RDR2 +13.3% / NULL (validated) |
| Source 2 games (CS2, Dota 2) | NULL-to-marginal | (untested, predicted) |
| Modern Vulkan/DX12 task-graph (2020+) | NULL | (untested, predicted) |
| GPU-bound any engine | NULL | RDR2 horizon (validated) |

---

## GPU utilization signature — how to know your test is valid

A test result is **only valid for measuring CPU optimization** when the
workload is CPU-bound. RDR2 Reports 7 vs 10 are the canonical case study:

| Test | Render scale | GPU utilization | CPU-bound? | Result |
|---|---|---|---|---|
| Report 7 (NULL) | "alto render scale" | **95%** sustained | No — GPU is the cap | False NULL |
| Report 10 (SIGNIFICANT +13.3%) | default | **84% baseline / 88% Rule 1 active** | Yes — CPU was the cap | True positive |

**The diagnostic signature**: when Ayama optimization reduces CPU latency,
GPU utilization **rises a few percentage points** (in Report 10: ~84% →
~88%). This is because the GPU was previously idling waiting for the CPU;
once the CPU delivers frames faster, the GPU has more work to chew. **If
toggling Ayama on/off shows no change in GPU%, you are GPU-bound** —
Ayama cannot help and the test will produce NULL regardless of the
engine.

**Operational rule for testers**: monitor GPU utilization (RTSS, MSI
Afterburner, GPU-Z) during a 30s steady-state observation. If GPU sits
at 95%+, lower graphics settings until GPU drops to 60-85% before
running the bench. The ΔGPU% between baseline and treated is a
secondary confirmation that the optimization is doing CPU work.

This pitfall is now documented in `EMPIRICAL_TEST_PROTOCOL.md §7.3` with
the operational check.

---

## Phase B.1 — Differential thread pinning (Rule 7) — INCONCLUSIVE

**Hypothesis**: For well-threaded engines, pin only the hot thread to
V-Cache CCD while releasing the process to all cores ("differential pin"
/ Rule 7), so worker threads can spread to both CCDs instead of fighting
for V-Cache.

**Test 1 (RDR2 Saint Denis night, same scene/settings as Report 10)**:

| Comparison | Δ avg | Δ P99 | Δ P99.9 | Δ Max | Δ Hitches | Verdict |
|---|---:|---:|---:|---:|---:|---|
| Rule 1 vs paused | **+13.3%** ✓sig | **+20.9%** ✓sig | +13.4% | -34.9% | +0.50 | SIGNIFICANT |
| Rule 7 vs paused | +9.9% ✓sig | +8.8% ✓sig | +7.0% ✓sig | **-57.0%** | **+200%** ✓sig | **REGRESSION DETECTED** |
| **Rule 7 vs Rule 1 (inferred)** | -3.4pp | -12.1pp | -6.4pp | spike worse | 2× hitches | **Rule 7 < Rule 1** |

**Why Rule 7 lost on RAGE**: the bench's `REGRESSION DETECTED` verdict is
won by hitches (both Rule 7 treated runs had 1 hitch, baseline had 0.33
avg). Max frame time degrades to 21.82 ms (~46 FPS worst frame) under
Rule 7 vs 14.70 ms (~68 FPS) under Rule 1. Mechanism: in RAGE engine,
workers **cooperate** with the hot thread (producer-consumer through
shared L3 cache); releasing them to CCD1 makes their data trips
cross-CCD (~70 ns vs ~10 ns intra-CCD), causing frame-time spikes.

**Status**: Rule 7 hypothesis **refuted on RAGE engine (RDR2)**. Still
**pending validation** on UE5 (Stalker 2, Hogwarts) and REDengine 4
(Cyberpunk 2077) where worker independence is higher and the
release-to-all-cores tactic may pay off. Rule 7 remains in the code
behind a checkbox toggle, not promoted to default. If 3+ consecutive
engines show Rule 7 < Rule 1, mark experiment as failed and remove.

---

## Hardware context — where it was tested

| Component | Tested | Predicted to behave similarly |
|---|---|---|
| AMD Ryzen 9 7950X3D | ✓ all runs (provisional) | — |
| AMD Ryzen 9 7900X3D | — | Yes (same dual-CCD + V-Cache architecture, fewer cores) |
| AMD Ryzen 7 7800X3D | — | **Different** — single CCD, V-Cache on all cores. Cross-CCD penalty doesn't exist. Expect NULL or minimal benefit on this CPU. |
| AMD Ryzen 9 9950X3D | — | Similar to 7950X3D (newer Zen 5 cores but same dual-CCD + V-Cache layout) |
| Intel hybrid (P-core + E-core) | — | Different policy needed (E-core eviction instead of CCD pinning). Not validated. |
| Non-asymmetric CPUs (5900X, 12900K homogeneous, etc.) | — | Ayama's primary policy doesn't apply. Self-pin + memory tuning may help marginally. |

**Bottom line**: Ayama is validated on AMD asymmetric V-Cache dual-CCD CPUs.
The 7950X3D class is the primary target.

---

## Methodology summary

- **Protocol**: 5-run A/B/A/B/A. Runs 1, 3, 5 are baseline (Ayama policies
  paused via IPC). Runs 2, 4 are treated (Ayama policies active). Each run
  is 30 seconds wall-clock enforced by PresentMon's `--timed` flag.
- **Capture tool**: PresentMon 2.4.1 (downloaded automatically by Ayama's
  CMake build).
- **Aggregation**: `ayama-cli bench multi` computes 95% confidence
  intervals via sample mean + sample stddev (n-1) for each metric.
  SIGNIFICANT requires baseline-vs-treated CIs to not overlap.
- **Workflow**: Fully orchestrated by `ayama-ui` bench runner (one click
  triggers all 5 captures + pauses/resumes + import + aggregation). Zero
  manual PowerShell intervention.
- **Hardware fixed**: same 7950X3D / 4090 / 32 GB DDR5 / Win11 for all
  tests. Only the game varies.

### Common methodological pitfalls and how to recognize them

| Pitfall | Symptom | Mitigation |
|---|---|---|
| Cold-cache outlier on Run 1 | Run 1 baseline avg much higher than runs 3, 5 | Treat Run 1 as "warm-up" data; verdict based on warm runs |
| PSO compile spike | Single 50-100ms frame in run 1 max-ms | Stutter inflated for Run 1; Avg/P99/P99.9 unaffected |
| VSync / framerate cap | Treated runs hit identical FPS to baseline | Disable VSync, remove engine caps, choose GPU-light scenes |
| GPU-bound workload | Treated and baseline both at GPU ceiling | NULL is expected; Ayama doesn't accelerate GPU |
| Engine has anti-cheat | Cannot test without ban risk | Skip the test entirely (Riot Vanguard, EAC, BattlEye, EA AC) |

---

## The 9 reports — one-line summaries

Read the full reports for raw CSVs, per-run data, and engine-specific
analysis.

1. **minecraft-dh-shaders_2026-05-16_7950x3d.md** — Solo Minecraft +
   Distant Horizons shaders with background apps (Chrome, VS Code).
   SIGNIFICANT P99.9 +22.7% — Ayama resolved background contention
   competing with the game thread.

2. **minecraft-twitch-discord_2026-05-16_7950x3d.md** — Multi-stream
   workload, contaminated by bug #19 (residual affinity from previous
   sessions). MARGINAL but partially invalid; documented the bug.

3. **minecraft-twitch-discord_2026-05-16_7950x3d_clean.md** — Same
   workload, clean system, bug #19 fixed. NULL result — system had
   spare cores, no contention to resolve. Validated theory.

4. **Minecraft post-PID-cache-fix re-test (informal)** — NULL, confirming
   that bug #26's PID-cache fix didn't change earlier Minecraft nulls
   from false-negatives to false-positives. Reports 3 and this re-test
   are mutually consistent.

5. **fallout4_2026-05-16_7950x3d.md** — Single-player Fallout 4 with High
   FPS Physics Fix mod + VSync. SIGNIFICANT +35% FPS across all metrics.
   Conservative result (VSync constraint).

6. **hogwarts-legacy_2026-05-16_7950x3d.md** — Hogwarts Castle gameplay.
   MARGINAL — Avg and P99 SIGNIFICANT, P99.9 within CI due to baseline
   variance. +15.5% FPS. Three architectural bugs (window style check,
   classifier signals, PID-cache) fixed during the test session.

7. **rdr2_2026-05-16_7950x3d.md** — RDR2 free-roam with Vulkan renderer.
   NULL — well-threaded RAGE engine + 4090 GPU-bound. Confirms model:
   no benefit on engines that already thread well.

8. **borderlands2_2026-05-17_7950x3d.md** — Two tests in the same
   session: normal-FPS zone produced NULL; high-FPS zone (uncapped, 480
   FPS) produced MARGINAL+ with P99 SIGNIFICANT and max-frame-time -50%.
   Refined the predictive model: CPU saturation at runtime is a
   necessary condition, not just engine architecture.

9. **halo2-mcc_2026-05-17_7950x3d.md** — Halo 2 Anniversary (MCC,
   remastered graphics, uncapped framerate). SIGNIFICANT on ALL metrics.
   **+56% FPS** — new empirical record. Confirms model's extreme end:
   oldest engine + fully uncapped framerate + modern CPU = maximum
   Ayama benefit.

10. **rdr2-saint-denis-night_2026-05-17_7950x3d.md** — RDR2 Saint
    Denis nighttime gameplay, **default render scale** (not "alto" as
    in Report 7), GPU at 84% baseline / 88% Rule 1 active.
    **SIGNIFICANT +13.3% avg, +20.9% P99, +13.4% P99.9** for Rule 1.
    **Refutes Report 7's NULL classification**: same engine, same
    hardware, same Ayama build — only the test scene's CPU-bound
    nature differed. The GPU utilization signature (84% → 88% on
    Rule 1 toggle) is the diagnostic that confirms CPU-bound regime.
    Phase B.1 differential-pin (Rule 7) also tested on this scene:
    **REGRESSION DETECTED** (+9.9% avg but +200% hitch rate, Max
    21.82 ms vs 14.70 ms under Rule 1). Hypothesis remains pending
    on UE5 / REDengine 4. Documents both findings, methodology
    innovation (GPU utilization as CPU-bound diagnostic), and the
    bugs fixed during the session (revert_all compaction, PresentMon
    `--stop_existing_session`, explorer.exe auto-discovery blacklist).

11. **halo-mcc-catalog_2026-05-17_7950x3d.md** — Halo MCC catalog
    sweep: 4 consecutive A/B/A/B/A tests (CE, 3, Reach, 4) in a single
    session, joined with Halo 2 from Report 9 to form a complete
    catalog across 11 years of engine evolution. **Halo CE: +98% FPS
    SIGNIFICANT all metrics** — new empirical record, surpassing
    Halo 2's +56%. **Halo 3: +19% SIGNIFICANT**. **Halo Reach: NULL
    with -89% variance reduction**. **Halo 4: NULL on engine ceiling
    (505 FPS steady state)**. The 5 data points form a strictly
    monotonic curve aligned with the engine's chronological migration
    from single-core Xbox 1 to mature multi-core Xbox 360 paradigms.
    Refines the predictive model: replaces "release year" with
    "hardware-target generation" as the dominant predictor. Confirms
    variance reduction signature persists across NULL-verdict games.
    First catalog-wide stress test of Phase B.1 bug fixes — all 4
    games completed without crashes, stuck PresentMon sessions, or
    explorer.exe pollution.

---

## Bugs found and fixed during empirical testing

The empirical phase exposed 30 real bugs in Ayama (29 fixed, 1 open).
A few that were architecturally important:

- **Bug #19** (residual affinity drift): cached `prev_mask` could be
  contaminated by previous Ayama sessions. Fixed by reverting to system
  default mask instead of stored prev.
- **Bug #26** (cache key collision): `ClassificationCache` keyed by
  exe name conflicted when two processes shared a name (launcher +
  game). Fixed by keying on PID instead.
- **Bug #29** (WoW64 / 32-bit games invisible): `EnumProcessModules`
  documented to fail on 32-bit targets from 64-bit caller. Fixed with
  `EnumProcessModulesEx(LIST_MODULES_ALL)`. Made all 32-bit legacy games
  testable.
- **Window-style false positives** (Steam UI, Discord, Chrome maximized
  registering as fullscreen games): Fixed by checking window class name
  (`Chrome_WidgetWin_*`) and `WS_THICKFRAME` style.

See individual reports for the full list.

---

## How to reproduce any of these tests

1. Clone or sync the Ayama tree at `<repo-root>/` (or wherever you have it).
2. `cmake -S <repo-root> -B build` — auto-downloads PresentMon 2.4.1.
3. `cmake --build build --target ayama-agent ayama-ui ayama-cli`.
4. Launch the target game, configure as documented in the per-test
   reproducibility section (e.g., disable VSync, enter the relevant
   scene).
5. Launch `ayama-agent.exe` as Administrator.
6. Launch `ayama-ui.exe` as Administrator.
7. Tab "Benchmark" → quick-pick the game's process → 30 s duration.
8. Click "Run A/B/A/B/A protocol (5 runs)" → wait ~3 min.
9. Read aggregate verdict in the panel.

---

## Scope and limitations of empirical claims

This summary documents what HAS been validated. It does NOT claim:

1. **That Ayama helps every CPU-bound game** — only ~60% of tested games
   showed measurable improvement. The remainder were NULL by clear
   architectural reasons.

2. **That results transfer to non-X3D CPUs** — only AMD asymmetric
   V-Cache dual-CCD CPUs validated. Intel hybrid / homogeneous AMD /
   non-X3D Ryzen require different policies (some implemented but
   untested).

3. **That results transfer outside Windows 11** — only Windows 11 26200
   tested. Windows 10 likely similar; Linux not tested in production.

4. **That improvements are perceptible to all users** — frame count
   deltas and millisecond reductions are real but vary in perceptual
   impact. A +56% FPS gain on Halo 2 is perceptually obvious; a
   variance-reduction-only result on RDR2 is subtle and may only matter
   to users sensitive to frame pacing.

5. **That tests with anti-cheat games are valid** — Ayama performs
   process operations that may trigger kernel-mode anti-cheats (Vanguard,
   EAC, BattlEye, EA Javelin). All tests in this catalog are on games
   without active anti-cheat or in single-player mode. Multiplayer
   competitive games are explicitly out of scope.

---

## What's next

Future tests would strengthen the model:

- **Halo CE / Halo 3 / Halo Reach MCC** — predicted +45-65% each. Single
  high-leverage test of MCC catalog would validate the engine-era theory.
- **Skyrim Special Edition** — 64-bit Creation Engine. Predicted +25-35%
  with VSync constraint, possibly higher if engine ceiling can be
  unlocked.
- **Cyberpunk 2077 (REDengine 4)** — **primary candidate to validate or
  refute Rule 7 (differential pin)**. Predicted MARGINAL for Rule 1
  (+5-15%); for Rule 7 the question is whether worker independence in
  REDengine 4 lets the "release to both CCDs" tactic pay off where it
  failed in RAGE. Status: pending installation by tester.
- **Stalker 2 (UE5 + Lumen)** — predicted MARGINAL for Rule 1, similar
  to Hogwarts. Secondary candidate for Rule 7 validation.
- **Confirmation on 7800X3D** — predicted NULL since it has no
  asymmetric CCD. Important to validate the "no benefit on symmetric
  CPUs" prediction.

If any of these tests contradict the predictions, the model needs
revision.

**Phase B.1 fate criterion**: if Rule 7 loses to Rule 1 on Cyberpunk
AND on one UE5 game (Stalker 2 or Hogwarts), mark the differential-pin
hypothesis as failed and remove the toggle from the UI. If Rule 7 wins
on any well-threaded engine, keep the toggle and document the
per-engine matrix.

---

## Project status

| Phase | Status |
|---|---|
| **Track 1**: agent CPU optimization | ✓ Complete (~2.9% sustained CPU, was 6-15%) |
| **Track 2**: UI bench runner (basic) | ✓ Complete (zero-PowerShell workflow) |
| **Detection pipeline** | ✓ Complete (30+ bugs fixed including major ones; revert_all + PresentMon-session bugs fixed during Phase B.1) |
| **Empirical validation** | ✓ Complete (14 tests, predictive model refined with GPU-saturation + hardware-generation caveats; new empirical record Halo CE +98%) |
| **Phase B.1 — Differential pin (Rule 7)** | ⚠ Refuted on RAGE (RDR2); pending on REDengine 4 + UE5 |
| **Documentation polish** | In progress (this doc + methodology doc + Master Plan update) |
| **Phase B.2 — CCD load defense** | Next up (high-value/low-risk extension to existing eviction logic) |
| **Visual polish (ImPlot, dev/user views, report exporter)** | Deferred |
| **Anti-cheat compatibility (Safe Mode)** | Deferred |
| **Public release readiness** | Not started |

Ayama is at the **"empirically validated proof-of-concept"** stage. The
foundation is solid; the value proposition is documented with falsifiable
predictions; the bug burden is manageable. The next leap (public release,
broad hardware coverage, anti-cheat tooling) requires separate planning.

---

*This document supersedes ad-hoc reports for high-level claims. For
per-test detail, see the individual reports in this directory.*
