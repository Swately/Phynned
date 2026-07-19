<!-- Authorship: researched & written by an LLM session (Opus) for Swately / Phynned, 2026-07-17. -->

# SOTA — Cache Science for Phynned

**Central question (operator):** *Games benefit from V-Cache. Could a non-game
application benefit the same way — and can we MEASURE which process needs the
expanded cache, per-process, from software on Windows, with no custom kernel driver?*

**Short answers, up front:**

1. **Yes — several non-game workload classes benefit as much as or more than games**
   (PS3 emulation, in-memory databases, HPC/CFD, EDA, shader compilation). The
   benefit is *workload-intrinsic*, not *application-category* intrinsic — it is
   governed by whether the working set crosses the 32 MB→96 MB L3 boundary and by
   memory-controller/bus pressure, not by "is it a game."
2. **Per-process cache-sensitivity IS partially measurable from user/admin software
   with no custom driver** — via ETW core-PMC counting attached to context-switch
   events (built-in OS facility, admin only). But there are hard limits: (a) L3-slice
   and memory-bandwidth counters are *per-CCX/per-socket*, not per-process; (b) only
   ~3–4 counters at once; (c) whether Zen 4's LLC-miss profile source is actually
   populated under Windows' HAL **must be confirmed empirically on the 7950X3D**.
3. **The empirical A/B route (place on CCD0, measure; place on CCD1, measure) is the
   most robust and most honest** path for arbitrary non-game processes, and it is
   exactly what the contention-aware-scheduling literature and the Linux cache-aware
   scheduler converged on. But that literature also warns: the biggest wins are in
   *QoS / consistency / energy for individual apps*, not average throughput.

Target hardware convention (this doc): **CCD0 = V-Cache die** (96 MB L3, lower clocks);
**CCD1 = frequency die** (32 MB L3, higher clocks). Note: source reviews are
*inconsistent* about which physical die is "CCD0" vs "CCD1" [V3]; the operator's
convention (CCD0 = V-Cache) is used throughout and is authoritative for Phynned.

---

## 1. Why V-Cache wins, and which non-game workloads measurably benefit / lose

### 1.1 The mechanism: the MPKI cliff at the working-set boundary

The V-Cache payoff is a **step function**, not a smooth gradient. A memory-centric
characterization of SPEC CPU2017 [V1 — arxiv 1910.00651, read via pdftotext] plots
MPKI (misses per kilo-instructions) vs cache size and finds:

- **Most workloads show a smooth exponential MPKI decrease** as cache grows — these
  get a modest, diminishing return from more L3. [V1]
- **Some workloads show a *cliff*:** "incrementally increasing cache size gives no
  significant improvements … until a point of saturation is reached. At this step, a
  sudden drop in the MPKI is observed." Named examples: **bwaves, lbm**; workloads
  with *multiple* working sets (multiple cliffs): **xalancbmk, nab, fotonik3d, lbm**. [V1]
- **The cliff location = the working-set size.** V-Cache pays exactly when a workload's
  working set sits *above 32 MB (CCD1's L3) but at/below 96 MB (CCD0's L3)* — the extra
  cache captures the drop. If the WS fits in 32 MB already, or vastly exceeds 96 MB,
  V-Cache does little. [V1 — inference from the cliff data]
- **"Most workloads suffer cache misses even with a reasonable 32 MB cache"** and ~50%
  of dynamic instructions are memory operations [V1] — i.e. the headroom for a bigger
  L3 to help is real and common.

**Cache-sensitive, large-working-set (benefit from more L3 / are memory-bound):**
`mcf`, `lbm`, `cactuBSSN`, `xz` — "fail to accommodate within the range of cache sizes …
refer the off-chip memory and hence affect the bandwidth"; they also have the **lowest
IPC** in the suite. [V1]

**Cache-insensitive (fit easily / low cache need → prefer clocks):** `povray`, `imagick`,
`nab`, `perlbench`, `exchange2`, `leela`, `namd`, `wrf`, and low-RSS `xalancbmk` —
"limited need for cache capacity, can be well executed without … main memory." [V1]

> Corollary for Phynned: a large working set is **necessary but not sufficient** for
> V-Cache to help. A *streaming* large-WS workload (`lbm`) is bandwidth-bound and does
> **not** benefit from more cache (its WS dwarfs any L3) — it just wants bandwidth. The
> sweet spot is *pointer-chasing / reuse-heavy* workloads whose WS lands in the 32–96 MB
> window. This is why "big WS" alone is a weak predictor (see §3). [V1 + V1 Blagodurov]

### 1.2 Measured per-application evidence (non-game)

**Desktop 7950X3D vs 7950X (non-X3D, higher clocks), content creation** — Puget Systems
[V2 — numbers via WebFetch extraction of the Puget review; not a full first-hand read]:

| Workload | 7950X (non-X3D) | 7950X3D | Winner |
|---|---|---|---|
| **UE5.1 shader compilation** | 1583.84 s | **1332.21 s** | **X3D +~16–19%** |
| DaVinci Resolve | 2793 | 2633 | non-X3D +6% |
| Cinebench R23 MT | 36,978 | 35,493 | non-X3D +4% |
| Blender (render) | 584.1 | 565.6 | non-X3D +3% |
| Premiere / After Effects / Photoshop / Lightroom | — | — | tie (~1–2%) |

Puget's own conclusion: recommend the non-X3D 7950X for content creation **except
shader-compilation workloads**. [V2]

- **General code compilation / compression / video encode:** ≈ **−5%** on 7950X3D vs
  7950X; the 7950X is ~6% faster in threaded apps, ~5% in single-thread — "all of its
  cores can clock higher." [V2/V3 — TechSpot & Tom's Hardware review summaries via search]
  → *Ordinary compilation prefers clocks; shader compilation (huge working set, heavy
  reuse) prefers cache.* Same tool category, opposite answer — proof the benefit is
  workload-intrinsic.
- **7-Zip:** ~**+10%** on 9950X3D "as the extra cache keeps more of the dictionary in
  fast L3." [V3 — TechBenchPro summary]

**Emulation** — PS3 (RPCS3) is latency-bound with heavy inter-thread communication and
uses an LLVM recompiler; "benefits from faster cache and memory speeds." Widely reported
as a large V-Cache winner, though I could **not** fetch the TechPowerUp per-FPS table
first-hand (403). [V3 — RPCS3/TechPowerUp secondary]

**Server / HPC (the strongest non-game evidence, biggest L3):** EPYC 9684X *Genoa-X*
(1152 MB L3 total, 96 MB/CCD) vs EPYC 9654 (Genoa, no V-Cache):
- **OpenFOAM (CFD): +49–52%**; drivaerFastback model +~30%. [V3 — Phoronix review summary]
- **Redis / in-memory DB: +145% throughput, +63% power efficiency** (9684X vs 9654) —
  "hot data fits into the large L3, higher L3 hit rate reduces DRAM access." [V3 —
  secondary blog; not verified against a primary benchmark table — treat as directional]

**Verdict for RQ1:** The workload classes that *measurably* benefit are **in-memory
databases, HPC/CFD (OpenFOAM), EDA/simulation, PS3 emulation, and shader compilation** —
all reuse-heavy with a 32–96 MB working set. The classes that measurably **lose** to the
frequency CCD are **CPU renderers (Cinebench/Blender), video encode, ordinary code
compilation, and small-WS/streaming work** — clock-bound, WS either < 32 MB or bandwidth-
bound. This directly answers the operator: **yes, non-game apps benefit exactly like
games do — for the same physical reason — and the answer flips per workload, not per
category.**

---

## 2. Per-process cache-sensitivity measurement on Windows (user/admin, NO custom driver)

This is the load-bearing engineering question. Ranked options below.

### 2.1 ETW core-PMC counting on context switches — the best driver-free path [V1]

**What it is:** Windows' built-in Event Tracing for Windows can attach hardware PMC
values to kernel ETW events. When counters are logged **on `CSwitch` (context-switch)
events**, the per-CPU counter delta between switches is **attributed to the thread being
switched *out*** — giving genuine **per-thread → per-process** attribution. [V1 — Bruce
Dawson/randomascii; V1 — Microsoft Learn "Recording PMU Events"]

**Official mechanism** [V1 — Microsoft Learn, read verbatim]:
- Enumerate counters: `wpr -pmcsources` or `xperf -pmcsources`.
- Counting mode (xperf): `xperf -on … -pmc InstructionRetired,TotalCycles CSWITCH strict`
  — "latest Windows Performance Analyzer will display the **Cycles per Instruction**
  table" when taken with `InstructionRetired,TotalCycles` on `CSwitch` + the
  `ProcessThread`, `Loader`, `CSwitch` keywords. [V1]
- Sampling mode (on PMC overflow): `xperf -on …+pmc_profile -pmcprofile instructionretired`
  or WPR `<SampledCounters>` with an `Interval` (events between samples). The
  `PerfInfoPMCSample` event "contains the id of the process and id of the counter." [V1 —
  MS Learn + Adam Sitnik]

**Counters exposed (architectural profile sources)** [V1 — Easyperf, tracelog help output]:
`BranchInstructions, BranchMispredictions, CacheMisses, TotalIssues, TotalCycles,
UnhaltedCoreCycles, InstructionRetired, UnhaltedReferenceCycles, LLCReference, LLCMisses,
BranchInstructionRetired, BranchMispredictsRetired`. MS Learn also shows `L3CacheAccess`
as a counter example. **`LLCReference` + `LLCMisses` + `InstructionRetired` = per-process
LLC-MPKI is, in principle, directly obtainable.** [V1]

**Privilege:** Administrator (elevated ETW kernel/system session). "You must be elevated
(Admin) to use ETW Kernel Session." [V1 — Adam Sitnik] No third-party or custom kernel
driver is installed — ETW is part of the OS. This is the decisive advantage over uProf/PCM.

**Hard limits (all verified):**
- **Simultaneous counter cap:** small. "For my machine, it's three" — exceeding it fails
  *silently* (later counters never fire) [V1 — Sitnik]. MS notes CPUs provide e.g. "four
  generic programmable counters and three fixed counters"; you must not over-subscribe. [V1]
- **Counting only counts while the thread runs**, and "IRQs and DPCs run in the context
  of a process, which means that they dirty the performance counters" — attribution is
  approximate. [V1 — Dawson]
- **Only a small subset of vendor PMU events is in the Windows HAL by default**; non-
  architectural AMD events must be registered via a WPRP `<MicroArchitecturalConfig>`
  (Event + Unit from the AMD datasheet) or registry — still no custom *driver*. [V1 — MS Learn]

> **⚠ CRITICAL UNVERIFIED RISK (must test on the actual box):** I could **not** verify
> first-hand that `LLCMisses`/`CacheMisses` are *populated* profile sources on **Zen 4
> (7950X3D) under Windows' HAL** — the counter *names* exist in the API, but which are
> live is vendor- and HAL-dependent, and Windows exposes "only a small subset by
> default." **Decisive empirical check: run `wpr -pmcsources` (or `xperf -pmcsources`) on
> the 7950X3D and record exactly which sources appear.** This is a 30-second test and it
> gates the entire PMU-based design. [V2 — gap declared]

### 2.2 AMD uProf — needs a kernel driver; L3/bandwidth are NOT per-process [V1/V2]

- **Requires a signed kernel driver on Windows:** `AMDPowerProfiler.sys` (historically
  hit Windows HLK-signing failures — "Windows cannot verify the digital signature").
  Runs as admin/services. [V2 — AMD community + uProf guide summaries] → **disqualifies
  it under the operator's "no custom kernel driver" constraint.**
- **Counter granularity** [V1 — docs.amd.com uProf §4.2, read via WebFetch]: **Core PMC =
  per logical core; L3 PMC = per L3 cache (i.e. per-CCX); DF (Data Fabric) PMC = per
  socket.** → **L3-miss and memory-bandwidth (Data Fabric) events are per-CCX / per-
  socket, physically shared — they cannot be attributed to an individual process.** Only
  core-PMC events and IBS (Instruction-Based Sampling, per-instruction with a PID) can be
  attributed to a process. [V1]

This is a fundamental hardware fact, not a tooling limitation: **the actual L3-slice and
memory-controller counters are shared resources.** Per-process "L3 miss rate" from
software is therefore always a *core-side approximation* (the misses that core issued),
never the true L3-occupancy of that process. Applies to ETW's `LLCMisses` too.

### 2.3 Intel PCM — driver-bound and Intel-only (N/A for 7950X3D) [V1]

- Requires `msr.sys` (must be **self-signed** on 64-bit Windows — `bcdedit /set
  testsigning on`) or the experimental third-party **WinRing0** driver; runs as
  Administrator. [V1 — intel/pcm WINDOWS_HOWTO.md, read via raw.githubusercontent]
- Intel-only (RDT: CMT/MBM per-RMID). **State plainly: irrelevant to the AMD 7950X3D.**
  Its per-process bandwidth story (RMID tagging) has no AMD-Windows equivalent without a
  driver.

### 2.4 OS-native, unprivileged IPC / cycle proxies [V1]

- **`QueryProcessCycleTime`** — "the number of CPU clock cycles used by the threads of the
  process … includes user and kernel." Needs only `PROCESS_QUERY_INFORMATION` **or
  `PROCESS_QUERY_LIMITED_INFORMATION`** (obtainable for most processes without full
  debug rights). Cycle count is captured via `rdtsc` at each context switch — accurate,
  unaffected by voluntary sleeps (unlike `GetProcessTimes`). [V1 — MS Learn]
- **`QueryThreadCycleTime`** (per-thread), **`QueryIdleProcessorCycleTime`** (per-logical-
  processor idle cycles → system busy/idle per core). [V1]
- **Use:** without a retired-instruction PMU counter you *cannot* compute true CPI, but
  `cycles / wall-time` and `cycles / unit-of-work` are free, near-zero-overhead, and
  unprivileged. High cycles with low forward progress (few frames/ops/bytes) is a
  memory-bound *hint*. Best used as the **A/B measurement instrument** (§4), not a
  standalone sensitivity predictor.

### 2.5 Ranking (privilege / overhead / per-process attribution quality)

| Option | Privilege | Overhead | Per-process attribution | Gives cache-miss rate? |
|---|---|---|---|---|
| **ETW core-PMC on CSwitch (counting)** | Admin, no driver | Low | **Good** (per-thread, delta on switch-out) | **Yes** (`LLCMisses`/`LLCReference` — *if HAL exposes them on Zen 4; unverified*) |
| ETW PMC sampling (overflow) | Admin, no driver | Low–med | Good (statistical; sample carries PID) | Yes (sampled) |
| `QueryProcessCycleTime` + work counter | **Low** (LIMITED_INFO) | ~0 | Exact (cycles) | No (cycles only; CPI proxy needs PMU) |
| Page faults / WS size (§3) | Low | ~0 | Exact | No (weak indirect proxy) |
| AMD uProf | Admin + **AMDPowerProfiler.sys driver** | Med | Core/IBS per-process; **L3/DF per-CCX/socket** | Core-side yes; true L3 no |
| Intel PCM | Admin + **msr.sys/WinRing0 driver** | Med | Per-core/socket (RDT per-RMID) | Intel-only — **N/A on AMD** |

**Recommendation:** primary = **ETW core-PMC counting** (`LLCReference`, `LLCMisses`,
`InstructionRetired`, `TotalCycles` on `CSwitch`), gated by the `-pmcsources` reality
check on the 7950X3D; fallback / always-on cheap layer = **`QueryProcessCycleTime` +
behavioral proxies (§3)**; ground truth = **A/B placement (§4)**. Never uProf/PCM under
the no-driver rule.

---

## 3. Indirect / behavioral predictors when PMU is unavailable

All obtainable cheaply and near-unprivileged (`GetProcessMemoryInfo` /
`K32GetProcessMemoryInfo`, `NtQuerySystemInformation`):

- **Working-set size trajectory** (`WorkingSetSize`, `PrivateUsage`): the single most
  useful free signal, because it locates the process relative to the **32 MB / 96 MB**
  boundaries. WS in the 32–96 MB band = candidate V-Cache winner; WS ≪ 32 MB = clock-
  bound; WS ≫ 96 MB = likely bandwidth-bound (V-Cache may not help). But WS alone
  cannot distinguish *reuse-heavy* (benefits) from *streaming* (doesn't) — `lbm` has a
  huge WS and is cache-*insensitive*. [V1 — SPEC2017; V1 — Blagodurov "devils"]
- **Page-fault rate** (`PageFaultCount`): **weak** proxy — page faults are DRAM↔disk /
  demand-zero events, a *different layer* from L3↔DRAM misses. High minor-fault churn
  indicates memory pressure, not cache sensitivity. Do not conflate. [V1 — GetProcessMemoryInfo semantics]
- **CPI estimate:** impossible without a retired-instruction counter; `QueryProcessCycleTime`
  gives cycles but not instructions. Cycles-per-wall-second normalized by a throughput
  signal is the closest free surrogate. [V1]
- **Thread-migration / cross-CCD churn** (observe scheduling via ETW `CSwitch` or
  `GetThreadIdealProcessor`): frequent cross-CCD migration *destroys* the V-Cache benefit
  (cold cache on arrival) — a behavioral signal that *placement itself* is the problem.

**How well do proxies predict sensitivity?** The contention literature is blunt: **the
real miss counter beats every model.** Blagodurov/Zhuravlev/Fedorova [V1] found the
**LLC miss rate is "one of the most accurate predictors"** of co-scheduling harm and
that it *outperforms* sophisticated stack-distance/reuse-distance models — surprisingly,
because the dominant cost is **memory-controller/bus/prefetch contention, not cache-space
contention**, and miss rate captures all of it. They explicitly refuted the intuition
that low-reuse ("devil") apps are insensitive: "devils were some of the **most
sensitive** applications." [V1] **Implication:** behavioral proxies (WS, faults) are a
*coarse pre-filter*; they cannot replace a miss-rate measurement or an A/B test for the
sensitivity verdict.

---

## 4. The A/B empirical route — measure benefit by experiment

For an *arbitrary non-game* process there is no FPS. The robust protocol mirrors
Phynned's existing 5-run A/B (docs/EMPIRICAL_TEST_PROTOCOL.md) but swaps the metric:

**Metric choices, best → weakest:**
1. **Cycles per unit of work** = `ΔQueryProcessCycleTime / work_done`. **Cycles is the
   cleanest cross-CCD metric** because it removes the frequency confound — CCD0 clocks
   lower, so wall-time unfairly penalizes the V-Cache die; *cycles-to-complete-the-same-
   work* isolates the cache effect. Requires a per-process "work" signal (bytes
   processed, transactions, frames, iterations — app-specific; use I/O counters
   `GetProcessIoCounters` or throughput logs as generic proxies). [V1 — QueryProcessCycleTime]
2. **Wall-clock throughput** (ops/s, or time-to-complete a fixed batch): what the user
   feels, but confounds cache benefit with the CCD clock delta. Report alongside cycles.
3. **Energy / energy-delay** (RAPL via MSR — needs a driver; or ETW
   `Microsoft-Windows-Kernel-Processor-Power`): closes the loop and is where the
   literature says cache-aware placement wins most (below).

**Prior art on online experimental placement:**

- **Contention-aware scheduling (Blagodurov/Zhuravlev/Fedorova, ACM TOCS 2010)** [V1]:
  *Distributed Intensity (DI)* spreads high-LLC-miss-rate threads across caches; *DI
  Online (DIO)* "dynamically reads miss counters online and schedules in real time."
  Classification-scheme ranking: **Pain metric** (sensitivity × intensity) best — within
  **1%** of optimal; **Miss Rate** "performs almost as well as Pain … and is by far the
  easiest to compute either online or offline"; DI overall "within 2% of the optimal";
  best-vs-worst schedule = **20%** average completion-time gap, **up to 50%** for
  individual apps; **DIO-POWER** improves energy-delay by **up to 80%**. [V1]
  > **The honest caveat that most constrains Phynned's mass-routing ambition** [V1]:
  > "if one is trying to improve **average** workload performance, the default
  > contention-unaware scheduler already does a rather good job." The real value of
  > contention-aware placement is **QoS / performance isolation for individual
  > applications and energy**, not aggregate throughput. Phynned should set expectations
  > accordingly: *consistency and per-app wins, not fleet-wide average speedups.*

- **Linux Cache-Aware Scheduling (CAS), 2024–2026, on 9950X3D/7950X3D** [V1/V2 — Phoronix]:
  BIOS `CPPC = Driver` + sysfs mode toggles between **cache** (pack tasks onto the
  V-Cache CCD) and **frequency** (pack onto the high-clock CCD). Measured on Linux 6.18:
  **most desktop workloads unchanged; PostgreSQL notably improved** (better throughput +
  lower latency); some DaCapo Java (Avrora, Tomcat, Tradesoap, Zxing) and nginx/network
  gains; a few workloads *regressed* vs mainline. [V1 — Phoronix result set] Patch
  authorship (AMD / K Prateek Nayak) I could **not** confirm first-hand — [V2, gap].
  > This is direct, recent prior art that **validates Phynned's empirical stance**: even
  > a kernel-resident cache-aware scheduler produces *workload-dependent, frequently null*
  > results. There is no shortcut around measuring the specific process.

- **Heuristic to port:** the DIO/CAS core idea — *use the online LLC-miss-rate (or its
  A/B cycle-count proxy) to decide cache-CCD vs frequency-CCD placement, per process,
  and re-evaluate periodically* — is precisely Phynned's "mass automatic core-routing."
  The novel-but-modest contribution available to Phynned is doing it **per-process on
  Windows from user/admin space**, which neither AMD's stack nor the Linux scheduler does.

---

## 5. How AMD's own stack decides (and why it leaves room for Phynned)

**Components** [V2/V3 — HotHardware/Tom's/AMD-community/VRChat-wiki summaries]:
- **AMD 3D V-Cache Performance Optimizer driver** + **AMD PPM Provisioning File Driver**,
  shipped in the chipset driver package (installed even on single-CCD parts).
- The Optimizer "dynamically modif[ies] the preferred cores based on workload preference
  for either the high-clocking die or the cache-heavy chiplet." The Provisioning driver
  "park[s] the secondary CCX cores when a **game is detected**." [V3]

**The actual decision signal:** **Xbox Game Bar's game detection.** "When a Ryzen 7000X3D
goes into 'game mode', it's because Xbox Game Bar detects a game is running and puts the
workload on the CCD with the AMD 3D V-Cache." Requires **Game Mode = On**, an updated
**Xbox Game Bar**, and BIOS **`CPPC Dynamic Preferred Cores = DRIVER`**. A manual
"Remember this is a game" toggle exists but is "finicky … may not appear every time." [V3]

**Limits (the gap Phynned fills):**
- It is a **binary game / not-game classifier**, keyed on Game Bar heuristics
  (known-title list + fullscreen), **not a measurement of cache sensitivity.** No PMU,
  no MPKI, no per-app benefit test. [V3]
- **Non-game apps default to the frequency CCD** regardless of whether they are
  cache-sensitive — so an RPCS3, a PostgreSQL, an OpenFOAM, or a shader-compile run (all
  of which *measurably* prefer V-Cache, §1) is **left on the wrong die** by AMD's stack. [V3 + V1/V2/V3 §1]
- Cross-CCD scheduling under this driver has been buggy in the field (core-parking
  failures needing `CPPC=Driver` + GFXOFF fixes). [V3]

**Therefore:** AMD answers "is a game fullscreen? → park CCD1." Phynned's differentiated
answer is "**does *this specific process* measurably run faster in cycles-per-work on the
V-Cache die? → route it there**," extended to *any* process, decided by measurement.
That is a real, unoccupied niche — but §4's caveat holds: expect per-app consistency and
correctly-routed outliers, not a universal average uplift.

---

## Decisive findings (each tagged)

1. V-Cache benefit is a **step function at the working-set/L3 boundary** — SPEC2017 shows an MPKI "cliff" where more cache does nothing until saturation, then MPKI drops suddenly (bwaves, lbm; multi-cliff: xalancbmk, nab, fotonik3d). [V1]
2. The V-Cache sweet spot = **working set > 32 MB (CCD1) and ≤ 96 MB (CCD0)** with high reuse; below 32 MB → clock-bound, far above 96 MB → bandwidth-bound. [V1, inference]
3. **Large working set is necessary but NOT sufficient** — streaming `lbm` has a huge WS yet is cache-insensitive; reuse pattern decides. [V1]
4. Cache-sensitive SPEC classes: `mcf, lbm, cactuBSSN, xz` (lowest IPC, off-chip-bound). Insensitive: `povray, imagick, nab, perlbench, exchange2, leela, namd, wrf`. [V1]
5. **Measured non-game winner — UE5.1 shader compilation: 7950X3D ~16–19% faster** than 7950X, the *only* content-creation win in Puget's suite. [V2]
6. **Measured non-game losers — DaVinci Resolve −6%, Cinebench R23 −4%, Blender −3%** on 7950X3D vs 7950X; general compile/compress/encode ≈ **−5%**. [V2/V3]
7. **Ordinary code compilation prefers clocks (−5%); shader compilation prefers cache (+~18%)** — same category, opposite verdict → benefit is workload-, not app-category-intrinsic. [V2/V3]
8. Server/HPC V-Cache is dramatic: **EPYC 9684X OpenFOAM +49–52%** vs 9654; Redis ~+145% throughput (directional). [V3]
9. **PS3 emulation (RPCS3)** is a strong V-Cache winner (latency + inter-thread comm + LLVM recompiler); per-FPS table not fetched first-hand. [V3]
10. **ETW core-PMC counting on `CSwitch` gives per-process attribution with NO custom driver** — delta attributed to the thread switched out; admin only. This is the key enabler. [V1]
11. Exposed ETW counters include **`LLCReference`, `LLCMisses`, `InstructionRetired`, `TotalCycles`, `CacheMisses`, `L3CacheAccess`** → per-process LLC-MPKI is *in principle* obtainable. [V1]
12. **UNVERIFIED, must test on the 7950X3D:** whether `LLCMisses`/`CacheMisses` are *live* profile sources under Windows' HAL on Zen 4 — Windows exposes "only a small subset by default." Run `wpr -pmcsources`. [V2, gap]
13. ETW PMC hard limit: **~3–4 counters simultaneously**, over-subscription fails *silently*; IRQ/DPC "dirty" the counters. [V1]
14. **AMD uProf requires the `AMDPowerProfiler.sys` kernel driver on Windows** (HLK-signing issues historically) → disqualified under the no-driver rule. [V2]
15. **On AMD hardware, L3-slice PMCs are per-CCX and Data-Fabric (bandwidth) PMCs are per-socket — physically shared, NOT per-process.** True per-process L3-miss rate is impossible from software; only a core-side approximation exists. [V1]
16. **Intel PCM needs `msr.sys` (self-signed) or WinRing0, admin, and is Intel-only** — no bearing on the 7950X3D. [V1]
17. **`QueryProcessCycleTime`** returns raw per-process cycles (user+kernel) via `rdtsc`-at-context-switch, needs only `PROCESS_QUERY_LIMITED_INFORMATION` → cheap, near-unprivileged A/B instrument. [V1]
18. **Page-fault rate is a weak cache proxy** (DRAM↔disk layer ≠ L3↔DRAM layer); working-set size is the better free signal because it locates the process vs the 32/96 MB boundary. [V1]
19. **LLC miss rate is the best single predictor of cache/contention sensitivity** and beats stack-distance/reuse models — because memory-controller/bus/prefetch contention dominates, not cache-space contention. [V1 — Blagodurov/Zhuravlev/Fedorova]
20. **"Devils" (high miss, low reuse) are the MOST sensitive, not insensitive** — refutes the naive "low reuse ⇒ won't care about cache" heuristic. [V1]
21. Best contention classifier = **Pain (sensitivity × intensity), within 1% of optimal**; **Miss Rate within a hair of it and far cheaper to compute online**; DI within 2% of optimal. [V1]
22. **Cross-CCD placement's biggest measured win is QoS/isolation for individual apps + energy (energy-delay +80%), NOT average throughput** — the default scheduler already does fine on averages. Phynned should promise consistency + correct outliers, not fleet uplift. [V1]
23. **For A/B on non-games, cycles-per-unit-work is the cleanest metric** — it removes the CCD0-clocks-lower confound that pollutes wall-time comparisons. [V1, inference from QPCT semantics]
24. **Linux Cache-Aware Scheduling (2024–2026) on 9950X3D/7950X3D:** most desktop workloads unchanged, **PostgreSQL notably improved**, some Java/network gains, a few regressions — recent prior art proving results are workload-dependent and often null. [V1/V2]
25. AMD's stack decides via **Xbox Game Bar game detection → park the frequency CCD**; requires Game Mode + updated Game Bar + `CPPC=DRIVER`. It is binary game/not-game, uses no PMU, and **leaves cache-sensitive non-games on the wrong die.** [V3]
26. **Phynned's unoccupied niche:** per-process, measurement-driven CCD routing on Windows from user/admin space — something neither AMD's driver (game-detection only) nor the Linux scheduler (kernel-resident, CCD-level) does. [V1/V2/V3 synthesis]

---

## Sources

**Per-process PMU / measurement (Windows):**
- Microsoft Learn — Recording Hardware Performance (PMU) Events: https://learn.microsoft.com/en-us/windows-hardware/test/wpt/recording-pmu-events [V1, read verbatim]
- Bruce Dawson (randomascii) — CPU Performance Counters on Windows: https://randomascii.wordpress.com/2016/11/27/cpu-performance-counters-on-windows/ [V1]
- Adam Sitnik — Collecting Hardware Performance Counters with ETW: https://adamsitnik.com/Hardware-Counters-ETW/ [V1]
- Easyperf — How to collect CPU performance counters on Windows: https://easyperf.net/blog/2019/02/23/How-to-collect-performance-counters-on-Windows [V1]
- Microsoft Learn — QueryProcessCycleTime: https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryprocesscycletime [V1]
- Intel PCM — Windows HOWTO (driver requirement): https://github.com/intel/pcm/blob/master/doc/WINDOWS_HOWTO.md [V1, via raw.githubusercontent]
- AMD uProf User Guide §4.2 PMC (Core/L3/DF granularity): https://docs.amd.com/r/en-US/57368-uProf-user-guide/4.2.-Performance-Monitoring-Counters-PMC [V1]
- AMD uProf — install/driver (AMDPowerProfiler.sys, signing issues): https://community.amd.com/t5/server-gurus-discussions/amd-uprof-services-failed-to-start-because-quot-windows-cannot/m-p/379078 ; https://docs.amd.com/r/en-US/57368-uProf-user-guide/Installing-AMD-uProf [V2]

**Cache science / V-Cache workload characterization:**
- Memory-Centric Characterization of SPEC CPU2017 (arxiv 1910.00651) — MPKI cliffs, working sets: https://arxiv.org/pdf/1910.00651 [V1, read via pdftotext]
- Puget Systems — Ryzen 9 7900X3D & 7950X3D Content Creation Review: https://www.pugetsystems.com/labs/articles/amd-ryzen-9-7900x3d-and-7950x3d-content-creation-review/ [V2]
- TechSpot — Ryzen 9 7950X3D Review: https://www.techspot.com/review/2636-amd-ryzen-7950x3d/ [V3, review 403 on fetch — via search summary]
- Tom's Hardware — Ryzen 9 7950X3D Review (productivity pages): https://www.tomshardware.com/reviews/amd-ryzen-9-7950x3d-cpu-review/7 [V3]
- Phoronix — EPYC 9684X Genoa-X HPC (OpenFOAM +49–52%): https://www.phoronix.com/review/amd-epyc-9684x-benchmarks/2 [V3]
- AMD 3D V-Cache server analysis (Redis/EPYC directional): https://pfat.cc/en/posts/2025-11-19-amd-3d-v-cache-from-gaming-cpu-king-to-server-performance-revolution/ [V3]
- TechPowerUp — 7800X3D emulation (RPCS3/Switch): https://www.techpowerup.com/review/amd-ryzen-7-7800x3d/16.html [V3, 403 on fetch]
- SkatterBencher #56 — 7950X3D per-CCD Fmax/voltage: https://skatterbencher.com/2023/02/27/skatterbencher-56-amd-ryzen-9-7950x3d-overclocked-to-5900-mhz/ [V3]

**Contention-aware scheduling / placement heuristics:**
- Blagodurov, Zhuravlev, Fedorova — Contention-Aware Scheduling on Multicore Systems, ACM TOCS 28(4) 2010: https://www.blagodurov.net/files/a8-blagodurov.pdf [V1, read via pdftotext]
- Fedorova et al. — Managing Contention for Shared Resources, ACM Queue: https://queue.acm.org/detail.cfm?id=1709862 [V3, 403 on fetch — corroborates the TOCS paper]
- Phoronix — Cache Aware Scheduling on Ryzen 9 9950X3D: https://www.phoronix.com/news/Ryzen-9950X3D-Cache-Aware-Sched [V1/V2]

**AMD 3D V-Cache Optimizer / Game Bar detection:**
- HotHardware — AMD chipset driver optimizes V-Cache: https://hothardware.com/news/amds-chipset-driver-for-v-cache-cpus [V3]
- Tom's Hardware — AMD updates 3D V-Cache optimizer driver: https://www.tomshardware.com/pc-components/cpus/amd-updates-3d-v-cache-optimizer-driver-ahead-of-ryzen-9000x3d-launch [V3, 403 on fetch]
- Hardware Times — enable Xbox Game Bar on 7900X3D/7950X3D: https://hardwaretimes.com/amd-enable-the-xbox-game-bar-on-the-ryzen-9-7900x3d-7950x3d-processors-for-better-performance/ [V3]
- VRChat Wiki — AMD X3D Series Processors guide: https://wiki.vrchat.com/wiki/Guides:AMD_X3D_Series_Processors [V3]

---

### Verification notes / declared gaps

- **[V1]** = primary read first-hand (MS Learn PMU & QPCT docs read as full verbatim
  markdown; SPEC2017 and Blagodurov papers read via local `pdftotext`; Intel PCM & uProf
  §4.2 read via WebFetch full content).
- **[V2]** = primary partial: content came from the primary source but through a WebFetch
  extraction layer, not my own full read (Puget per-app numbers; uProf driver name;
  Phoronix CAS authorship gap).
- **[V3]** = secondary only (review summaries via search where the review page returned
  403: TechSpot, Tom's Hardware, TechPowerUp, Phoronix EPYC page; the PFAT Redis figure).
- **The single load-bearing unverified item** is Finding #12: whether Zen 4's LLC-miss
  ETW profile sources are live on the 7950X3D under the current Windows HAL. Resolve it
  first, empirically, with `wpr -pmcsources` on the actual machine — it gates the whole
  PMU design and is a 30-second test.
- Number without a tagged source appears nowhere in this document by intent.

<!-- Made with my soul - Swately <3 -->
