<!-- AAP A0 artifact — the frozen objective for the Phynned mass-router architecture search. -->
<!-- Per protocols/analysis/ARCHITECTURE_ANALYSIS_PROTOCOL.md §3.A0. Once frozen, this block -->
<!-- is byte-intact through the whole run (AINV-1/AINV-12); re-opening = a deliberate return to A0. -->

# Phynned Mass-Router — Frozen Objective (AAP A0)

Status: **FROZEN 2026-07-17** · AT0 verdict: `FREEZE WITH FIXES` → the three point-fixes
(M1a attribution method, M1c audio instrument, E8 time-bound clarification) applied and the
objective frozen by the supervisor. Process note: the AT0 adversary (Sonnet) also wrote its
M1a fix directly into the file, exceeding its verdict-only mandate; the supervisor read the
full objective first-hand and takes ownership of the frozen text regardless of who typed each
line. From this point the §1–§7 content is byte-intact through the run (AINV-1/AINV-12).

**RE-OPEN 2026-07-17 (deliberate operator amendment — the only sanctioned way to change a
frozen objective, AAP §3.A0 / §8; the operator is above the loop).** After the anti-cheat
research ([`../research/SOTA_ANTICHEAT_AFFINITY.md`](../research/SOTA_ANTICHEAT_AFFINITY.md))
resolved the (a)/(b)/(c) AC-behaviour split, the operator directed a shift from conservative
abstention to a **zero-handle AC-driver detection + least-privilege single cached probe**
(try-then-back-off). **K1 and M1d are amended accordingly** (marked inline); no metric,
priority, envelope, or other kill criterion changed. This is a recorded re-open, not a stealth
edit — the pre-amendment text is preserved in git history (commit before this edit) and the
change is scoped to the two AC items only.
· AAP run: the protocol's second
independent-domain instance (first: Holith/FPGA). Supervisor: the session; operator: Swately.
Evidence base: the five SOTA dossiers in [`../research/`](../research/) (control-plane, cache
science, prior art, safety/compat, event engine — every load-bearing claim [V]-tagged there)
plus two on-box measurements taken 2026-07-17: the PMC source listing (§4.1) and the prior
in-repo NtQSI cost figure.

---

## §1 — The mission (one paragraph)

Design the architecture that turns Phynned into a **mass, automatic, measurement-driven CPU
placement service** for Windows on asymmetric CPUs (first target: 7950X3D dual-CCD): every
*touchable* process gets placed on the cores it benefits from — cache-sensitive work toward
the V-Cache CCD, clock-sensitive work toward the frequency CCD, the background herd penned
away from both — at near-zero agent cost, without breaking anything, and with every placement
decision **explainable and backed by measurement** (the niche prior art leaves open: nobody on
Windows routes by *measured* per-process cache benefit, and nobody publishes per-app A/B
evidence).

## §2 — Measurable success metrics + the explicit priority ordering

**M1 — Safety (the QUALITY VETO — outranks everything; per PERFECTION_TRADEOFF_DOCTRINE):**
- M1a: a ≥4-hour soak on the operator's live desktop (game + OBS + Discord + browser + the
  ~300-process background) produces **zero** allowlist violations in the audit log and zero
  attributable crashes/hangs. **Attribution method:** a crash/hang counts against the agent
  iff the failing process appears in the agent's audit log with an action applied within the
  trailing 10 minutes before the failure (Reliability Monitor / WER records cross-referenced
  against `audit.bin` timestamps); failures in never-touched processes are incidental.
- M1b: `taskkill /f` on the agent followed by restart **reverts 100% of journaled placements**
  (measured by comparing pre/post masks — affinity persists after setter death, so RAII revert
  is insufficient by [V1] evidence).
- M1c: zero audio defects during the soak while audio plays (audiodg-class incidents are the
  documented top folk-failure). **Instrument (primary):** the ETW audio-glitch signal —
  Microsoft-Windows-Audio glitch events / audiodg GlitchCount — sampled across the soak, must
  not rise above its agent-off baseline; **secondary/confirmatory:** operator live-listening
  during the game+OBS+Discord segment. A rise in the counter OR an operator-reported dropout
  fails M1c.
- M1d: (amended 2026-07-17 — see the header re-open note) the audit log shows **zero handle
  opens** on any foreground game where a **(c) monitor-and-report or unrecognized** anti-cheat
  driver is detected active; for titles with **no AC or a known (a)/(b)** AC driver, at most
  **one** least-privilege probe per exe-identity, labeled ALLOWED/BLOCKED.

**M2 — Efficacy (measured, not asserted):**
- M2a: on the named workload set (§5), the router's placement **agrees with the A/B-measured
  best placement** (ground truth: cycles-per-unit-work via QueryProcessCycleTime per §4.1) in
  the majority of cases, reported as an agreement percentage — target ≥70%, floor ≥50%
  (below the floor the router is a coin toss and fails).
- M2b: **at least one reproduced double-digit improvement on a non-game workload** (the
  shader-compile / emulator / database class the cache dossier documents as V-Cache winners).
- M2c: **do-no-harm bound:** no workload in the set regresses >2% under the router vs
  Windows-default placement (same A/B instrument).

**M3 — Agent cost (the operator's "lo más barato" mandate, hard ceilings):**
- M3a: steady-state agent CPU **<0.5% of one core** averaged over 10 min (SelfMonitor
  instrument, already in-tree).
- M3b: agent RSS **<20 MB** (UI excluded — separate process).
- M3c: birth→placement latency **p95 ≤500 ms** (ETW birth timestamp → Set* call timestamp,
  logged; the user-mode ceiling is a few ms *after* first schedule — pre-first-instruction
  placement is kernel-driver territory and out of envelope).

**M4 — Evidence (the differentiator):**
- M4a: every placement decision carries a logged, human-readable reason (rule hit or
  measurement values) — auditable end-to-end.
- M4b: the system generates a per-app A/B report automatically (the bench module extended),
  publishable per the project's evidence discipline.

**Priority ordering (explicit, frozen):** `M1 = veto` (a design failing any M1 item is dead
regardless of other scores) → then **M2 > M3 > M4** on any conflict. Tie-break inside a metric:
the sub-items in listed order. Rationale: an unsafe router is a product-killer; an accurate
router that costs 2% CPU still beats a free router that mis-places (the cost ceilings are
hard, but M2 wins *design* conflicts under those ceilings); evidence is the moat but serves
efficacy, not the reverse.

## §3 — Feasibility envelope (hard constraints)

- **E1:** elevated user-mode ONLY. **No custom kernel driver, no test-signing** — neither for
  placement nor for measurement.
- **E2:** Windows 11 on the operator's 7950X3D (dual CCD: 96 MB V-Cache @ lower clocks / 32 MB
  @ higher clocks). The design must **degrade gracefully** on homogeneous/single-CCD topologies
  (monitor-only), but 7950X3D is the target that must win.
- **E3:** implementation base = the existing Phynned codebase (C++23, MinGW GCC, CMake;
  modules observer/policy/action/learn/bench/ipc/config/core + the vendored `framework/`).
  New modules are allowed; abandoning the base for a greenfield project is not (check-the-base
  rule). The **ETW layer's known defects (wrong provider GUIDs, missing system-logger mode,
  ignored return codes, uint8_t slot ceiling) are prerequisite fixes inside this arc** — any
  candidate depending on ETW inherits that repair as scoped work, not as an assumption.
- **E4:** the control mechanisms available are exactly the [V1]-verified set of the
  control-plane dossier: hard affinity masks (inherit to children; beat CPU Sets), CPU Sets
  (soft; no child inheritance; `PROCESS_SET_LIMITED_INFORMATION` suffices), Job Objects
  (O(1) group policy; membership is one-way), priority classes, EcoQoS/power throttling,
  memory priority. The composability law — *hard affinity silently beats CPU Sets* — is a
  design input: layers must be core-disjoint or explicitly reasoned.
- **E5:** **coexistence:** the design must detect the AMD 3D V-Cache Optimizer service,
  Process Lasso, and Windows Game Mode, and defer/bound its own actions to avoid policy
  flapping (two-optimizers-fighting is a documented failure class).
- **E6:** the **allowlist reframe** is binding (safety dossier F0): the mass layer acts on an
  allowlist of touchable classes, with the expanded denylist (audiodg, ctfmon, conhost, vmmem,
  EDR/AC service tables, PPL, self-managed-mask processes…) as second line of defence — never
  "touch everything except exceptions".
- **E7:** measurement instruments available on the box (verified 2026-07-17, §4.1): ETW PMC
  sources including `CacheMisses`, `DcacheMisses/Accesses`, `InstructionRetired`,
  `TotalCycles` (max 6 simultaneous; LLC-level mapping on Zen 4 **unconfirmed** — requires
  empirical calibration); `QueryProcessCycleTime` (near-unprivileged); NtQSI bulk snapshot
  (~80-150 µs at ~200 processes, in-repo measurement); working-set positioning vs the
  32/96 MB boundaries.
- **E8:** budget: the operator's single box + LLM-assisted sessions. **There is no hard
  calendar/session ceiling; staged milestones each with a measurable exit on the real box are
  the deliberate substitute for a time bound** (AAP A4 requires a staged path to real numbers).
  A design is out of envelope only if it has no such staged, measurable milestone path — not
  for taking many sessions.

## §4 — On-box measurements anchoring this objective

### §4.1 — PMC sources (run 2026-07-17, `wpr -pmcsources`, elevated)
Available: Timer, TotalIssues, BranchInstructions, DcacheMisses, IcacheMisses,
BranchMispredictions, IcacheIssues, DcacheAccesses, TotalCycles, CacheMisses,
InstructionRetired, ICFetch, ICMiss, FRRetiredx86Instructions, FRRetiredBranches,
FRRetiredBranchesMispredicted, DCAccess. **Maximum 6 selectable simultaneously.**
No source is labeled LLC-anything: the level `CacheMisses` maps to on Zen 4 is
**could-not-verify** from the listing — candidates using it must include a calibration step
and the A/B route as ground-truth arbiter (this is why M2's instrument is cycles-per-unit-work,
not MPKI).

## §5 — Named workload set (the concrete cases; AT0 check 5)

1. **Shader-compile-class** (UE5-style shader compilation — the documented ~16-19% non-game
   V-Cache winner) — must be routed to CCD0 and measured.
2. **Emulator-class** (RPCS3-like) — documented V-Cache winner class.
3. **Ordinary code compilation** (documented ~-5% on cache CCD → must be routed to CCD1/clocks).
4. **The streamer mix** (game + OBS + Discord + browser + background) — the herd case; the
   game layer stays untouched (AMD driver's job), the herd gets penned, audio stays clean.
5. **Idle desktop flood** (~300 background processes) — the cost case: M3 ceilings hold here.

## §6 — Kill criteria (each falsifiable; any hit disqualifies a candidate)

- **K1:** (amended 2026-07-17 — see the header re-open note; access right corrected 2026-07-17b)
  disqualifies — (a) opening **any** handle on a foreground game where a **(c) or unrecognized**
  AC driver is detected active; (b) re-probing an exe already labeled BLOCKED; (c) any access
  right **beyond `PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION` (0x1200)** — in
  particular any of `PROCESS_VM_READ`/`VM_WRITE`/`PROCESS_QUERY_INFORMATION`/`ALL_ACCESS` (the
  memory/heavy rights anti-cheat actually watches); or requiring the weakening of AC in any way.
  A **single** probe with the 0x1200 handle on a title with no AC / a known (a)/(b) AC driver is
  the permitted mechanism. (0x1000 QUERY_LIMITED is required because reading the affinity mask —
  for the no-op probe set and the DR1 revert journal — is impossible with SET-only; verified
  on-box: 0x0200 alone fails `GetProcessAffinityMask` with ACCESS_DENIED. QUERY_LIMITED is not
  cheat-shaped and the probe must use the same handle the real router uses to be a valid test.)
- **K2:** cannot demonstrate journal-based full recovery after `taskkill /f` of the agent
  (M1b scenario) — placements must never be orphaned.
- **K3:** requires always-on full-rate context-switch tracing (or any component whose
  *designed* steady-state cost exceeds the M3 ceilings) — burst/on-demand tracing is fine.
- **K4:** its routing signal is not obtainable on the real box (§4.1) AND it lacks the A/B
  fallback — unmeasurable routing is advocacy.
- **K5:** needs a kernel driver / test-signing (violates E1).
- **K6:** cannot express or measure the do-no-harm bound (no per-decision revert path, no
  regression measurement).
- **K7:** one-design-in-costumes at A1 (not a candidate property but a search property —
  enforced by Gate 1).

## §7 — What this objective does NOT claim

It does not claim fleet-wide average throughput gains (prior art says wins live in outliers,
isolation/consistency and specific workload classes — the objective targets exactly those);
it does not claim the game-pinning layer as novel (commoditized; AMD's driver owns it); and
"optimal" anywhere in this run is scoped to *best among the candidates generated against this
objective* (AINV-11).

<!-- Made with my soul - Swately <3 -->
