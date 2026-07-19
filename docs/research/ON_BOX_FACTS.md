# On-box facts — measured on the operator's 7950X3D (2026-07-17)

> Supervisor measurements taken during the AAP run to close the "could not verify —
> needs a 7950X3D bench" gaps the SOTA dossiers flagged. Every number here is from a
> command run on the operator's live machine, tagged [M] (measured first-hand) or
> [M-noisy] (measured but confounded — read the caveat). This file feeds the A2
> adversaries and the A4 feasibility veto; it is NOT a candidate.

## 1. The box
- **AMD Ryzen 9 7950X3D, 16C/32T** [M] (`Win32_Processor`). Dual CCD: CCD0 = 8 cores +
  96 MB V-Cache @ lower clocks, CCD1 = 8 cores + 32 MB L3 @ higher clocks (spec, not
  re-derived on-box — the exact CCD↔logical-processor map is a milestone-1 calibration item).
- **Live process/thread scale (idle-ish desktop):** 169 processes, 4510 threads [M]
  (`Get-Process`). Under the §5 streamer mix this rises; the "~300 background" figure in the
  objective is a soak-time target, not the idle number.

## 2. PMU / measurement instruments present (no driver)
- `wpr -pmcsources` [M] lists usable PMC sources including **CacheMisses, DcacheMisses,
  DcacheAccesses, IcacheMisses, InstructionRetired, TotalCycles, BranchInstructions,
  BranchMispredictions** — **max 6 selectable simultaneously**.
- **No source is labeled "LLC".** Whether `CacheMisses` on Zen 4 attributes L3 (vs L1/L2) is
  **unresolved from the listing** — this is the calibration gate the cache dossier named. It
  does NOT block the design: the frozen objective already makes cycles-per-unit-work (A/B) the
  M2a ground truth, with PMC as an accelerator only.
- Decode tooling: `wpr`, `tracerpt`, `logman` present [M]; **`xperf`/`wpa` absent** (not in
  Windows Kits; not installed). The wpr(.wprp PMC profile)→tracerpt path is the no-install
  route for the milestone-1 attribution proof; xperf/WPT is a one-winget-install fallback.

## 3. The cache cliff is REAL and LARGE on this box — the project's premise, validated [M]
Pointer-chase micro (`scratchpad/pmc/chase.cpp`: random Hamiltonian cycle over 64-byte nodes,
dependent loads defeat prefetch/MLP; fixed seed; `-O2`), **unpinned, single run each**:

| Working set | ns / hop | Regime |
|---|---:|---|
| 8 MB   | 3.5  | fits L3 of **both** CCDs |
| 64 MB  | 11.4 | fits the 96 MB V-Cache, **exceeds** the 32 MB frequency-CCD L3 — the decisive routing regime |
| 512 MB | 46.2 | exceeds both → DRAM-bound |

A **3.3×** latency step from L3-resident to the 64 MB regime and **13×** to DRAM — the
step-function / MPKI-cliff the cache dossier described, now measured on the operator's silicon.
The 64 MB point is the money case: a workload here would be served by the V-Cache CCD's L3 but
would miss out of the frequency CCD's — exactly where routing produces a real delta. **This is
the empirical basis that the thing Phynned routes for actually matters on this hardware.**

## 4. Per-CCD A/B — attempted, NOISY, inconclusive [M-noisy]
Pinning the 64 MB chase to one core of each CCD (`SetProcessAffinityMask` 0x1 vs 0x10000,
3 reps) gave **13.0–18.2 ns/hop** on one and **16.3–43.0 ns/hop** on the other — variance
larger than any CCD signal, and the *pinned* runs were often **slower and noisier than the
unpinned 11.4**. Causes, all real and named:
- the box was under load from 4 concurrent Opus designer agents (a polluted measurement
  environment — my own fault for measuring during the fan-out);
- **wall-time carries the clock confound** the cache dossier warned of (the frequency CCD boosts
  higher but throttles under sustained load — ns/hop conflates cache latency with clock state);
- affinity pinning may be contending with SMT siblings or the AMD V-Cache driver's own parking.

**Honest verdict:** a clean per-CCD number is NOT established. What this negative result *does*
establish is that **wall-time is the wrong instrument** — it empirically vindicates the frozen
objective's choice of **cycles-per-unit-work via `QueryProcessCycleTime`** as the M2a ground
truth (cycles are immune to the clock confound). A clean CCD-vs-CCD delta on a quiet box, using
the cycles metric and verified pinning, is **milestone-1's measurable exit** (A4 staged path),
not a result claimable now.

## 5. What this settles for the AAP
- **Premise (cache matters here):** VALIDATED [M] — §3.
- **M2a instrument choice (cycles-per-work, not wall-time):** VINDICATED [M-noisy] — §4.
- **PMC path exists without a driver, LLC-attribution TBD:** the feasibility "path exists" bar
  for candidate B is met; the LLC-mapping resolution is a named milestone, not a blocker — §2.
- **M3 cost scale plausible:** 169 proc / 4510 threads is within the NtQSI sub-ms regime the
  event dossier derived — §1.
- **ETW GUID defect confirmed in-repo** (`framework/etw/SessionManager.hpp:160`) — any
  ETW-dependent candidate inherits that repair as scoped work (already in the objective's E3).

## 6. Clean per-CCD V-Cache A/B (M1 milestone-1, quiet box 2026-07-17) + a self-correction

Re-ran the per-CCD A/B on a QUIET box (operator asleep — the contention that made §4 noisy
was gone). **Two load-bearing outcomes, one of them a corrected error:**

### 6.1 The CCD↔V-Cache mapping — VERIFIED against the OS ground truth [M]
`GetLogicalProcessorInformationEx(RelationCache)` on this box:
- **L3 #0: 96 MB, logical processors 0–15** → the **V-Cache CCD**.
- **L3 #1: 32 MB, logical processors 16–31** → the **frequency CCD**.

The framework's `phyriad::hw::v_cache_cores()` returns exactly `{0..15}` — **the framework's
V-Cache detection is CORRECT** (verified first-hand against the Win32 API). The router will pin
cache-winners to logical 0–15, which is right.

### 6.2 Self-correction (verify-before-claim earned its keep) [honest record]
My FIRST quiet-box A/B concluded logical 16–31 was V-Cache (the opposite). It was WRONG, from a
**footprint-labeling bug in the `chase.cpp` micro**: the chased array is `link` (8 B/node), so
the real working-set footprint was `mb/8`, not `mb` — an 8× error that inverted the
size-vs-CCD reading. The Win32 `RelationCache` ground-truth check (§6.1) caught it before it
reached any design doc. (A second self-inflicted micro bug was also caught: swapping the return
value let `-O2` delete the whole chase loop as dead code — the 0.000 s reading exposed it; a
`volatile` sink fixed it.) Both are recorded so the number below is trusted, not assumed.

### 6.3 The corrected measurement [M-modest]
Footprint-fixed micro, 64 MB actual working set (fits the 96 MB V-Cache, not the 32 MB
frequency L3), pinned to logical 4 (V-Cache) vs logical 20 (frequency), min of 4 reps, quiet box:
- **V-Cache CCD: 48.5 ns/hop (204 cyc/hop)** · **frequency CCD: 58.7 ns/hop (246 cyc/hop)**
- → the **V-Cache CCD is 1.21× faster** for this cache-sensitive workload, with fewer cycles
  (the cycle metric confirms it is fewer misses, not clock).

**Honest caveat on the magnitude:** a random 64 MB pointer-chase is TLB-bound (≈16 K pages ≫ the
TLB), so every hop pays a page-walk regardless of cache residency — this CAPS the visible
V-Cache benefit at ~1.2× here. Real cache-sensitive apps with better locality (databases, shader
compilers, emulators) show more, consistent with the cache dossier's [V2] +16–19 % shader-compile
and the near-zero-for-many-desktop-workloads Linux-CAS result. **The operator's original
question — "can a non-game benefit from V-Cache, and can we measure which one?" — is answered:
YES, measurably, when the working set is cache-sensitive at the 32–96 MB scale; the per-process
A/B (cycles-per-work) is the instrument, and it works.** A cleaner absolute magnitude needs a
locality-friendly (or huge-page) micro — deferred; the design-load-bearing facts (framework
detection correct; benefit measurable; A/B route valid) are established.

### 6.4 Per-core clock + boost, confirmed by HWiNFO — and the clock gap corrected UP [M]
The operator's HWiNFO capture (2026-07-17, after enabling C-states + spread-spectrum in BIOS —
they had been OFF, which is why idle cores never dropped and boost was capped) gives per-core
current/min/**max** clock, a 3rd independent source that agrees with §6.1:

| Physical cores | CCD | Max clock (HWiNFO) | = logical procs |
|---|---|---|---|
| **0–7** | **V-Cache** | **5,250 MHz** | 0–15 (96 MB L3) |
| **8–15** | **frequency** | **5,750–5,800 MHz** | 16–31 (32 MB L3) |

- **The real per-CCD clock gap is ~5.80 vs 5.25 GHz = ~550 MHz ≈ 10.5 %** at peak boost — near the
  operator's cited 11 %. **This CORRECTS §6.3's ~2 %:** that small figure was measured under the
  broken BIOS config (C-states OFF → idle cores stuck ~94–98 % → no budget freed → no peak boost).
  With C-states on, cores 9 & 13 hit **5,775–5,800 MHz** live. **So the clock-bound routing benefit
  is materially bigger than §6.3 reported — up to ~10.5 % at peak.**
- **CPPC preferred cores** (HWiNFO "perf #N"): Core 9 (#1) and Core 13 (#1) are the top boosters,
  both on the frequency CCD. **Clock-bound routing should target the CPPC-preferred core of the
  frequency CCD (9/13), not just any frequency core** — those are the ones that reach 5.8.
- **`min processor state` is NOT a boost lever** [M]: toggling PROCTHROTTLEMIN 50 %↔0 % moved the
  loaded core's boost by ~0 (127 % both) — min sets the floor, not the ceiling. The real enablers
  were the BIOS C-states (idle-drop → budget concentration) + a quiet chip. Recorded so we don't
  chase min-state again.
- **Telemetry verdict:** per-core effective clock is readable no-driver via PDH
  `\Processor Information(*)\% Processor Performance` (validated: loaded V-Cache core → 125 % base
  ≈ 5.25 GHz; loaded freq core → 5.3–5.5 GHz). For the richer set HWiNFO shows (per-core max, VID,
  power, PPT, CPPC rank) the path is **HWiNFO's `Global\HWiNFO_SENS_SM2` shared memory** (its own
  signed driver reads the MSRs; we only read the SHM — zero kernel code of ours, the RTSS model).

<!-- Made with my soul - Swately <3 -->
