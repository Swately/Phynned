<!-- AAP DESIGNER artifact — one candidate architecture for the Phynned mass-router. -->
<!-- Angle D: MINIMAL-MECHANISM / coarse-grained herding via Job Objects + CPU Sets. -->
<!-- Written against the FROZEN OBJECTIVE (PHYNNED_MASS_ROUTER_OBJECTIVE.md), in deliberate -->
<!-- isolation from the other candidates. Conduct: verify-before-claim; numbers carry basis. -->
<!-- Authorship: LLM DESIGNER session (Opus) for Swately / Phynned, 2026-07-17. -->

# CAND_D — Minimal-Mechanism Coarse Herding (Job Objects + CPU Sets)

**One-line thesis.** Do the *cheapest thing that captures most of the benefit*: leave the game
to AMD's driver, route a **small static curated set** of known cache-winners onto the V-Cache
die with the lightest available call, and **pen the background herd off the V-Cache die** —
no per-process PMU in the hot path, measurement kept only as an occasional A/B validator. The
bet (grounded in the cache dossier's central finding): the real wins are **isolation /
consistency / energy for individual apps, not average throughput** — so ~80% of that isolation
benefit is reachable at ~5% of the mechanism cost, and *less mechanism means less to break (M1)
and less to run (M3).*

This candidate optimizes for **M3 (cost)** and **M1 (safety)** by construction, buys **M2b**
efficacy from a curated prior, and **openly concedes** the M2a agreement ceiling a coarse
signal can hit (§7). It never bends the frozen objective; where it under-serves a metric it
says so.

Convention (from the cache dossier, operator-authoritative): **CCD0 = V-Cache die** (96 MB L3,
lower clocks); **CCD1 = frequency die** (32 MB L3, higher clocks). All masks are derived at
runtime from `hw::v_cache_cores()` / `hw::ccd_cores()` — never hard-coded — because source
reviews disagree on physical CCD numbering [SOTA_CACHE_SCIENCE §Target-hardware].

---

## 1. Architecture — the bucket model

Every *touchable* process lands in exactly one bucket. The classification is **coarse on
purpose**: an exe-name table (curated + denylist) plus **one** cheap activity gate
(`cpu_usage_pct`, already collected free by the FR-11 bulk snapshot). No per-process cache
measurement gates any placement.

| Bucket | Membership test | Mechanism | Hardness | Right needed | Revertible? |
|---|---|---|---|---|---|
| **Game** | classifier → `Game` | **untouched** (AMD 3D V-Cache Optimizer + Game Bar own CCD0) | — | none (query-only to detect) | n/a |
| **Curated cache-winner** | exe ∈ `kCuratedCacheWinners` | `SetProcessDefaultCpuSetMasks(CCD0)` | **soft** | `PROCESS_SET_LIMITED_INFORMATION` | yes (clear/re-apply) |
| **Herd** | touchable ∧ not-curated ∧ **low-activity** (background) | `SetProcessDefaultCpuSetMasks(CCD1)` *(default)* → Job-Object hard pen *(escalation, §1.4)* | soft → hard | `PROCESS_SET_LIMITED_INFORMATION` → `PROCESS_SET_QUOTA\|TERMINATE` | yes → O(1) re-policy |
| **Active non-curated** | touchable ∧ not-curated ∧ **high-activity** MT | **untouched** (M1 baseline) → optional soft CCD1 bias (M2 milestone) | — → soft | — → `PROCESS_SET_LIMITED_INFORMATION` | n/a → yes |
| **Denylist** | exe ∈ deny tables ∨ PPL ∨ self-managed-mask | **untouched, never SET-opened** | — | none | n/a |

**Why this is "coarse two-bucket herding."** There are only two placement destinations —
**CCD0 (value)** and **CCD1 (herd)** — plus "leave alone." The V-Cache die gets a hand-curated
few; the frequency die gets the idle crowd; everything heavy-and-unlisted, plus the game and
the denylist, is left to the OS / AMD. That is the entire policy surface.

### 1.1 Why the buckets are core-disjoint by construction (the composability law)

The control-plane dossier's load-bearing rule: **a restrictive hard affinity mask silently
beats any conflicting CPU-Set assignment** [SOTA_CONTROL_PLANE §1.2(2), V1]. This candidate
never lets that rule bite, because:

- The **only hard-affinity actor** is the (escalation) herd Job, and its ceiling names **only
  CCD1 cores**.
- Every process *of value* is either on **CCD0** (curated, soft CPU-Set) or **untouched**
  (game via AMD, active work via OS).
- CCD0-cores and CCD1-cores are **disjoint sets**. A hard mask on CCD1 therefore *cannot*
  contradict a soft set on CCD0 — the two layers live on different silicon. The game's AMD-owned
  CCD0 placement and a curated winner's soft CCD0 set share the die by *intent* (both are
  V-Cache winners), and neither is a hard mask, so there is no silent override to reason about.

This is the whole reason the design can stay minimal: **disjoint destinations make the
composability law a non-event**, so we never need per-target conflict arbitration.

### 1.2 The O(1) control calls — stated precisely (verify-before-claim)

The angle's headline is "one O(1) policy call." The honest decomposition:

- **Job Object re-policy is genuinely O(1):** one `SetInformationJobObject(job,
  JobObjectBasicLimitInformation, LIMIT_AFFINITY=mask)` changes the affinity ceiling of *every*
  enrolled member at once — the only true one-call-governs-N primitive Windows exposes
  [SOTA_CONTROL_PLANE §1.3, §5, V1]. This is used for the whole-herd **revert** (widen to full
  mask in one call) and for any global re-policy.
- **Enrollment is O(N), for *both* mechanisms.** `AssignProcessToJobObject` is per-process, and
  `SetProcessDefaultCpuSetMasks` is per-process — there is *no* batch-across-PIDs enroll call
  [SOTA_CONTROL_PLANE §5, V1]. So populating the herd is O(N) light (CPU-Set) or O(N) heavy
  (Job) regardless.
- **Birth-driven placement makes enrollment amortized O(1)-per-newborn.** Because the design
  places each process *at its ETW birth event* (§1.5) — one process per event — the per-birth
  cost is one control op, whichever mechanism. The "O(1) for N processes" luxury (Job re-policy)
  is therefore **rarely exercised** on a fixed-topology box: the CCD layout never changes, so the
  herd is essentially never re-policied *except on revert.*

**Consequence for the mechanism choice (this is the key minimal-angle finding).** Since both
mechanisms are O(N) to enroll and enrollment is amortized O(1)-per-birth anyway, the Job Object's
one advantage (O(1) re-policy) buys little in steady state. Meanwhile the Job costs *more* per
member to enroll: `AssignProcessToJobObject` requires a handle with **`PROCESS_SET_QUOTA` +
`PROCESS_TERMINATE`** ([V3] — Process Security table hex from SOTA_CONTROL_PLANE §4.1; the exact
AssignProcessToJobObject requirement needs an on-box confirm), and **`PROCESS_TERMINATE` is
exactly the bit AV/EDR heuristics score hardest** on a mass handle-opener [SOTA_SAFETY_COMPAT
F19]. The CPU-Set path needs only **`PROCESS_SET_LIMITED_INFORMATION`** — the objective's named
low-privilege mass-router call — and "cannot fail when passed valid parameters"
[SOTA_CONTROL_PLANE §1.2, V1].

**Therefore the DEFAULT herd pen is the CPU-Set mask, and the Job Object is the measured
escalation (§1.4), not the baseline.** This keeps the angle's mechanism (Job Objects + CPU Sets,
coarse, O(1)-capable) while making the safe/light path the default — the correct call under the
M1 veto.

### 1.3 Curated cache-winner routing → CCD0 (soft CPU-Set)

`kCuratedCacheWinners` is a **static table of ~30 exe names** the cache dossier documents as
measured or strongly-evidenced V-Cache winners (all workload-intrinsic, WS in the 32–96 MB
reuse-heavy window) [SOTA_CACHE_SCIENCE §1, §Decisive-findings]:

- Shader compilation: `ShaderCompileWorker.exe`, `UnrealEditor.exe`/`UE4Editor.exe`/`UnrealPak`
  (UE5.1 shader compile: measured **+16–19%** on 7950X3D, the *only* content-creation win in
  Puget's suite [V2]).
- Emulators: `rpcs3.exe` (PS3 — strong latency/IPC winner [V3]), `vita3k.exe`, `xenia.exe`.
- In-memory / DB: `postgres.exe` (Linux CAS: *notably improved* [V1]), `redis-server.exe`.
- Compression with large dictionary: `7z.exe`, `7zG.exe` (~+10% [V3]).
- HPC/EDA solvers where the name is unambiguous (OpenFOAM `*Foam` solvers [V3]).

Placement is `SetProcessDefaultCpuSetMasks(pid, CCD0_group_affinity)` — **soft** (composes with
OS power management, does not strand threads, does not infect children, is freely revertible)
[SOTA_CONTROL_PLANE §1.2, V1]. Soft is correct here: a curated winner *prefers* CCD0 but must be
allowed to spill if CCD0 saturates, and its placement must revert cleanly (M1b).

Note on the losers: the cache dossier's documented **losers** (ordinary `cl.exe`/`clang`/`link`,
Cinebench, Blender, `ffmpeg`, DaVinci) are *deliberately not curated* → they fall to "active
non-curated → untouched," where the OS gives a wide MT build all 32 threads. That is the
**do-no-harm-correct** outcome (a `-j32` build wants all cores; penning it to one 16-thread CCD
would ~halve it and *fail M2c*), and it is obtained **for free** by simply not listing them.

### 1.4 Herd pen → CCD1: default soft, escalation hard

**Default (Milestone M1):** each *low-activity* touchable newborn gets
`SetProcessDefaultCpuSetMasks(pid, CCD1_group_affinity)` at birth. Low-activity is a **single
coarse gate**: `cpu_usage_pct` (free from the bulk snapshot) below a threshold → "background
herd." Penning a near-idle process to a 16-thread CCD **cannot regress it** (it uses <1 core),
so the do-no-harm risk on the herd is structurally near-zero, while its *memory traffic* is
steered off CCD0 — which is where the isolation win lives (keeping the background's cache
footprint out of the game's / curated-winner's V-Cache).

**Escalation (measured, optional milestone):** a soft CPU-Set is *advisory* — the scheduler may
still let herd threads take time on CCD0 under load ("non-exclusive by default"
[SOTA_CONTROL_PLANE §1.2, V1]). If the M2 isolation A/B measures that soft eviction is
insufficient (curated-winner or game still loses cache to the herd under load), escalate the
*idle subset* into **one Job Object** with `JOB_OBJECT_LIMIT_AFFINITY = CCD1` for a **guaranteed
hard pen** + O(1) re-policy. Accept the heavier enroll handle *only for that subset*, and only
because it was measured necessary. Hard affinity to a **whole 16-thread CCD** is safe: the F12
"most apps crash when core count drops" catastrophe is a **1–2-core** pathology
[SOTA_SAFETY_COMPAT F12]; 16 logical cores time-slice a herd's threads without stranding.

The Job's **one-way membership caveat** [SOTA_CONTROL_PLANE §1.3, V1/V3] is handled honestly in
§2 (M1b), never by pretending a process can be un-assigned.

### 1.5 Birth engine + reconcile (delta-driven, no full-table hot path)

- **Birth:** one ETW real-time session on **Microsoft-Windows-Kernel-Process**
  `{22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}`, keyword `0x10`. ProcessStart carries
  PID/ImageName/ParentPID — **enough to classify and route without any follow-up query**
  [SOTA_EVENT_ENGINE §1.2, V1]. This reuses the existing `phyriad::etw::SessionManager` verbatim
  **after the prerequisite GUID fix** (the current `kKernelProcess` GUID is wrong → births are
  very likely never delivered today [SOTA_EVENT_ENGINE F-1, E3]).
- **Reconcile janitor:** a full `NtQuerySystemInformation` pass every **2–5 s** catches missed
  births/exits and PID-recycle drift — **sub-millisecond even at 800 processes** (~0.3–0.6 ms
  DERIVED from the in-repo 80–150 µs @200 anchor [SOTA_EVENT_ENGINE §3.1]). The 100 ms tick does
  *not* scan all processes; it drains the ETW ring and updates the tiny active set.
- **No CSwitch session in steady state.** CSwitch is the dominant ETW cost (tens-of-thousands to
  >100k events/s [SOTA_EVENT_ENGINE §2.1]); this design's routing needs *only* the rare
  birth stream. CSwitch/PMU is spun up **on demand** solely for an A/B validation run, then
  stopped.

### 1.6 Module reuse / additions (deliberately SMALL)

**Reused as-is (zero change):** `phyriad::hw::set_process_affinity`/`get_process_affinity`
(FR-3), topology probe + `v_cache_cores()`/`ccd_cores()`/`ccd_count()` (CCD mask derivation),
`phyriad::etw::SessionManager` (birth engine, post-GUID-fix), `observer::ProcessClassifier`
(denylist tables + game detection + `ProcessInfo.cpu_usage_pct`), `action::ActionExecutor` +
`ActionLogRing`/`audit.bin` (audit + revert bookkeeping base), `core::AgentRuntime` (loop),
`SelfMonitor` (M3 instrument, already enforces <20 MB / <0.5%), `ipc` (SHM publish for UI),
`policy::PolicyDecision` (32 B decision POD).

**Additions (all small):**
1. `hw::set_process_default_cpuset_mask(pid, GROUP_AFFINITY)` + `hw::clear_process_default_cpuset(pid)`
   — thin FR wrappers over `SetProcessDefaultCpuSetMasks` (lightest right, cannot-fail). ~40 LOC
   beside FR-3.
2. `ProcessClassifier` extension: `kCuratedCacheWinners[]` (~30 names, §1.3), the denylist
   **deltas** the safety dossier requires (`audiodg.exe, ctfmon.exe, conhost.exe`, EDR name/path
   table, AC-service table, `vmmem/vmwp/vmcompute`, generic hosts) [SOTA_SAFETY_COMPAT
   Facet-1/2], and a one-line `is_background_herd(ProcessInfo)` activity gate. ~data + ~50 LOC.
3. `PlacementJournal` — small append-only on-disk record `{exe-identity, start-time, pid,
   mechanism, applied-mask}`, flushed on apply; the M1b SIGKILL recovery backbone the safety
   dossier calls for [SOTA_SAFETY_COMPAT F17]. ~120 LOC atop `audit.bin`.
4. `HerdJob` RAII wrapper (escalation only): CreateJobObject → Assign → SetInformation(LIMIT_
   AFFINITY) → O(1) revert-to-full → close **without** `KILL_ON_JOB_CLOSE`. ~80 LOC, built only
   for the escalation milestone.
5. `ABValidator` (bench-module extension): occasional `QueryProcessCycleTime`-per-work A/B; **not
   hot-path**. ~150 LOC.
6. **Shared prerequisites** (not unique to this candidate, per E3): fix the ETW GUIDs
   [F-1…F-4], widen `pid_hash` slot `uint8_t→uint16_t` and `kMaxTargets 64→1024` with the slim
   ~48 B/PID record [SOTA_EVENT_ENGINE §3.3/§3.5].

No new subsystem, no kernel driver, no per-process PMU pipeline. That is the whole point.

---

## 2. M1 — how each safety item is satisfied (the veto)

**M1a — zero allowlist violations, zero attributable crashes/hangs (4 h soak).**
- **Allowlist-first (F0), not deny-everything.** The *only* processes ever touched are (a) exe ∈
  `kCuratedCacheWinners` and (b) the low-activity herd — both positive-membership sets. The
  expanded denylist is the *second* line [SOTA_SAFETY_COMPAT F0]. Fewer touched processes = fewer
  ways to induce a crash — the minimal thesis paying M1 directly.
- **No hard-affinity-into-few-cores.** Default herd is a *soft* CPU-Set (cannot starve — the
  scheduler overrides it under load [SOTA_SAFETY_COMPAT F12 mitigation (a)]); the escalation Job's
  hard pen is a *whole 16-thread CCD*, not the 1–2-core F12 pathology.
- **Self-managed processes exempt (F13).** Any process whose affinity mask is **non-default at
  observe time** is treated as self-managing → never herded, never routed (matches the safety
  dossier's F13/F18 rule). This also sidesteps the SQL-Server-scheduler class of fights.
- **No priority/EcoQoS/memory-priority levers at all** (§5 drops) → the whole F14/EcoQoS
  instability class is *absent by construction*.

**M1b — `taskkill /f` then restart reverts 100% of journaled placements.**
The Job's one-way membership means revert is **not** per-process un-assignment. Honest design:
- **Graceful shutdown:** curated CPU-Sets cleared per-process (`clear_process_default_cpuset`,
  O(N_curated), tiny); if the escalation Job exists, **one** `SetInformationJobObject(full_mask)`
  widens the entire herd's ceiling in **O(1)**, then the job handle closes **without**
  `KILL_ON_JOB_CLOSE`.
- **SIGKILL (taskkill /f — C++ destructors do NOT run [SOTA_SAFETY_COMPAT F17]):** the guarantee
  is the **journal**, not RAII. On next start, *before any new action*, replay `PlacementJournal`:
  for each record whose target is still live (matched by **exe-identity + start-time**, PID-recycle
  safe), read its **current** affinity/CPU-Set; if it still equals our applied narrow value, revert
  it (`set_process_affinity(pid, full)` / `clear_process_default_cpuset`). CPU-Set placements
  revert cleanly per-process. For a killed escalation Job, membership ended when the agent's handle
  closed; whether members' affinity snapped back is **could-not-verify** from the docs, so recovery
  does **not** rely on it — it reconciles each journaled PID's *current* mask explicitly. Result:
  **100% of journaled placements revert**, satisfying M1b under the one-way caveat. (Reconciling
  against the live mask, not blind restore, also avoids the F18 "revert-to-default clobbers the
  app's own narrower mask" bug.)

**M1c — zero audio defects.**
- `audiodg.exe` on the denylist (the top MASS-arc gap [SOTA_SAFETY_COMPAT F2]) → never herded,
  never CPU-Set. Communication apps (Discord/Teams/Zoom — `kCommNames`) are *active* → untouched.
- **No EcoQoS anywhere** → the "EcoQoS on an audio graph = instability" class [F14] cannot occur.
- Instrument: the ETW audio-glitch counter (Microsoft-Windows-Audio / audiodg GlitchCount)
  sampled across the soak must not rise above the agent-off baseline (M1c primary), operator
  live-listen secondary. The minimal design's advantage: **it never touches the audio path**, so
  the expected counter delta is zero.

**M1d — anti-cheat processes and their entire descendant trees: zero SET-rights handle opens.**
- AC-service table on the denylist (`EasyAntiCheat*, BEService, vgc, vgtray`, …
  [SOTA_SAFETY_COMPAT F9/F11]).
- **Descendant-tree protection:** the launcher-inheritance trap [F10] is honored — launchers stay
  `kLauncherHelperNames` (untouched), and because the design **never touches a launcher or a game**,
  it cannot propagate a mask into a protected child (the Elden-Ring/EAC integrity-crash class).
  For the AC process tree, classification is **denylist-first, before any OpenProcess**: a process
  whose image path is under a security-vendor / AC directory, *or whose parent chain hits an AC
  service*, is skipped without ever requesting a SET-rights handle. The safest handle is the one
  never opened [F19] — and this design opens the *lightest* handle (`PROCESS_SET_LIMITED_
  INFORMATION`) on the *fewest* processes, never `PROCESS_VM_*`, never on lsass/EDR/AC.

**Kill-criteria check:** K1 (no SET-open on AC/descendants) ✓ via denylist-first; K2 (journal
recovery) ✓ §2 M1b; K3 (no always-on full-rate CSwitch) ✓ birth stream only, CSwitch on-demand;
K5 (no driver) ✓; K6 (do-no-harm expressible + per-decision revert) ✓ §3/§2; K7 (not a costume)
✓ — this is a genuinely distinct point in the space (static-prior + coarse gate + O(1) group pen
vs per-process measurement).

---

## 3. M2 — efficacy with a COARSE signal (measured honestly)

**M2b (≥1 reproduced double-digit non-game win) — this candidate's efficacy anchor.** The
curated set *is* the winner list: routing `ShaderCompileWorker`/UE5 to CCD0 reproduces the
Puget-measured **+16–19%** shader-compile win [SOTA_CACHE_SCIENCE §1.2, V2]; `rpcs3` and
`postgres` are secondary double-digit candidates. M2b is delivered by the curated prior, and the
`ABValidator` (cycles-per-unit-work, the frequency-confound-free metric [SOTA_CACHE_SCIENCE §4,
V1]) produces the reproduced number + the M4 report. This is the strongest efficacy leg — it
rides *published measurements*, not a hope.

**M2c (do-no-harm, no workload regresses >2%) — structurally protected.**
- Curated → CCD0 is **soft** (spills under load, no strand).
- Heavy active non-curated (Blender/`-j32` build/encode) is **untouched** → keeps all 32 cores →
  cannot regress from placement. This is the deliberate design choice that makes the coarse model
  safe: *the workloads most at risk of a placement regression are exactly the ones this design
  refuses to place.*
- Idle herd → CCD1: a process using <1 core cannot lose measurable throughput on a 16-thread CCD.
- Instrument: the same A/B cycles-per-work harness, run agent-on vs agent-off per §5 workload.

**M2a (agreement with A/B-measured best placement; target ≥70%, floor ≥50%) — the honest
ceiling.** The coarse router's placement per §5 workload:
- WL1 shader-compile → curated → CCD0 = A/B-best (CCD0). **AGREE.**
- WL2 emulator (RPCS3) → curated → CCD0 = A/B-best (CCD0). **AGREE.**
- WL3 ordinary compile → active non-curated → untouched (all cores). If A/B is three-way
  {CCD0, CCD1, all}, a wide build's best is *all cores* → **AGREE**; if A/B is strict two-way
  {CCD0 vs CCD1}, best is CCD1 and "untouched" scores as **not-CCD1 → partial/miss**. *This is
  the coarse penalty and it is real* (§7). The optional M2 milestone (soft CCD1 bias for active
  clock-bound work) converts this to AGREE if the strict-two-way scoring is used.
- WL4 streamer-mix → game untouched (AMD), herd penned, audio clean = the intended placement.
  **AGREE** on the herd/game split.
- WL5 idle flood → all herd → CCD1 = the isolation intent; for near-idle processes the A/B "best"
  is a near-coin-toss (placement barely matters), so agreement here hovers around the floor by
  nature.

**Honest M2a self-assessment:** on a §5 set dominated by curated + obvious-clock-bound + idle
cases, agreement clears the **≥50% floor comfortably and can approach ≥70%** if WL3 is scored
three-way or the M2 soft-bias milestone is added. But the coarse signal **structurally cannot
discover an un-listed cache winner** — the objective's own niche ("route by *measured*
per-process cache benefit"). Any cache-sensitive process not on `kCuratedCacheWinners` is herded
or left, and there the router *guesses* rather than *measures*. That caps M2a's headroom and is
named plainly in §7.

---

## 4. M3 — cost budget (this candidate's STRONG suit; the arithmetic)

All figures carry basis; the per-syscall constant is [V3]/DERIVED and flagged for on-box
measurement [SOTA_CONTROL_PLANE §5, SOTA_EVENT_ENGINE §4.1].

**Per-birth cost (the only hot path):**
- Classify: ~30 curated + ~40 denylist `strcmp`s on the short exe name ≈ **sub-µs**.
- One control op: `OpenProcess(PROCESS_SET_LIMITED_INFORMATION)` + `SetProcessDefaultCpuSetMasks`
  + `CloseHandle` ≈ **single-digit µs** [SOTA_EVENT_ENGINE §4.1, V2].
- **≈ a few µs per newborn.**

**Steady-state CPU (M3a ceiling <0.5% of one core / 10 min):**
- Birth rate on a desktop: a handful/s idle, bursts of dozens at launcher spawn. Take a
  pessimistic sustained **100 births/s × 5 µs = 500 µs/s = 0.05%** of one core.
- Reconcile janitor: ~0.6 ms every 2 s (@800 procs DERIVED) = **0.03%** of one core.
- No steady-state PMU / CSwitch = **0%** measurement cost.
- **Total ≈ 0.08% of one core — ~6× under the 0.5% ceiling**, with headroom for the reconcile to
  slow further under `SelfMonitor` breach-demotion. (Contrast a per-process-PMU design, which
  pays the CSwitch stream — tens of thousands of events/s — continuously.)

**Memory (M3b ceiling <20 MB RSS):**
- Slim ~48 B/PID × 1024 PIDs ≈ **~50 KB** state [SOTA_EVENT_ENGINE §3.3]. Birth ETW session
  buffers 64 KB × 16 = **1 MB**. Curated/journal tables: a few KB. The resident agent carries no
  UI/Vulkan deps (separate process) → **comfortably < 20 MB**, targeting the Process-Lasso-core
  bar of ~1–3 MB [SOTA_EVENT_ENGINE §5].

**Birth→placement latency (M3c p95 ≤500 ms):**
- Path = ETW ProcessStart delivery → one OpenProcess+Set. **No measure-then-decide round-trip**
  (the coarse latency advantage). User-mode floor after first schedule = a few ms
  [SOTA_EVENT_ENGINE §1.3].
- **The tail risk, stated honestly:** a lone low-volume birth session flushes on the FlushTimer
  (min **~1 s**) [SOTA_EVENT_ENGINE §2.4] — worst-case ~1 s, over the 500 ms p95 target. In
  practice, ambient process churn (svchost/conhost/tasks) fills the session's buffers faster than
  1 s, so **p95 is plausibly < 500 ms while the worst-case tail is ~1 s** — this must be
  **measured** (E8 milestone). Two levers if p95 fails: a secondary WNF birth signal (~immediate,
  no admin [SOTA_EVENT_ENGINE §1.6]) or riding a co-active CSwitch session's buffer fill.
  Load-bearing note: latency **only matters for short-lived cache-winners**, which barely exist —
  shader compiles/emulators/DBs run for minutes-to-hours, so 500 ms–1 s to place them is
  negligible against their runtime. The herd tolerates any latency.

**Cost bottom line:** **≈0.08% of one core, <20 MB RSS, birth-driven few-µs placements, zero
steady-state measurement** — M3a/M3b met with wide margin; M3c met in practice for the workloads
that matter, tail flagged for on-box confirmation.

---

## 5. What it DROPS (a lot, deliberately) + coexistence + milestones

**Dropped (each drop = less to break / less to run):**
- **Per-process PMU / LLC-miss counting in the hot path** — the whole ETW-CSwitch-PMU pipeline.
  Kept only as an occasional `ABValidator`. (Drops the objective's headline "measured per-process
  benefit" *for the mass layer* — the central concession, §7.)
- **Per-thread differential pinning** (the existing Rule 7) for the mass layer.
- **EcoQoS, priority-class changes, memory-priority** (SOTA §2 levers) — none used. Drops the
  entire F14 instability class and keeps the design *pure placement*.
- **The game-routing layer** — AMD's driver owns it, prior art says it is commoditized
  [SOTA_PRIOR_ART §8a]. We only *detect* the game.
- **Reactive per-tick placement / hysteresis control loop** — placement is *birth-once + reconcile*,
  never consumption-reactive, so the F15 oscillation/thrash class is absent by construction.
- **Per-game learned memory** (`learn/`) for the mass layer — the curated list is static.

**Coexistence (E5).** Detect the **AMD 3D V-Cache Optimizer service** (`amd3dvcacheSvc.exe`),
**Process Lasso** (`ProcessGovernor.exe`), and **Windows Game Mode** before acting
[SOTA_SAFETY_COMPAT F16, SOTA_PRIOR_ART §2/§3]. This design is *natively low-conflict*: it never
re-places what AMD places (game/CCD0-parking are untouched), so there is no tug-of-war on the
game die. The one interaction — the herd Job (escalation) keeping CCD1 unparked while AMD parks
it for a game — is *benign and intended* (herd works on CCD1, off the game's CCD0). If Process
Lasso Forced-Mode is detected on a target, **defer** (never ship a Forced-Mode equivalent).

**Staged milestones (E8/A4 — each a measurable on-box exit):**
- **M0 — prerequisites & reality-check.** Run `wpr -pmcsources` + `logman query providers
  Microsoft-Windows-Kernel-Process` on the 7950X3D; fix the ETW GUIDs [F-1…F-4]; widen the
  structural caps. **Exit:** ProcessStart events actually delivered (birth counter > 0); PMC
  source availability recorded for the validator.
- **M1 — safety-first (the veto, GREEN before anything else).** Curated CPU-Set→CCD0 + soft
  herd CPU-Set→CCD1 + denylist + `PlacementJournal`. **Exit:** 4 h live soak → zero allowlist
  violations, zero attributable crashes (Reliability-Monitor cross-ref), audio-glitch counter
  flat vs baseline; `taskkill /f` → journal reverts **100%** of placements.
- **M2 — efficacy.** A/B the curated winners (cycles-per-work) → reproduce ≥1 double-digit win
  (M2b); A/B the untouched/idle set → M2c ≤2%; compute M2a agreement on §5. **Exit:** M2b hit,
  M2c held, M2a ≥ floor. *If isolation A/B shows soft eviction insufficient → build the Job-pen
  escalation and re-measure.*
- **M3 — cost.** `SelfMonitor` over 10 min idle-flood → <0.5% CPU, <20 MB RSS; log birth→place
  latencies → p95. **Exit:** M3a/b measured green; M3c p95 characterized (tail lever engaged if
  needed).
- **M4 — evidence.** Per-decision reason string logged (rule hit / activity value); `ABValidator`
  auto-emits the per-app A/B report. **Exit:** end-to-end auditable; one published per-app report.

---

## 6. Coexistence with the priority ordering (M1=veto → M2>M3>M4)

This candidate is *built* along the priority gradient: M1 and M3 are satisfied by construction
(minimal mechanism), M2b is bought from a curated prior, and M2a is where it deliberately trades
headroom for safety+cost. Under the frozen tie-break (M2 wins *design* conflicts under the hard
M3 ceilings), the one place this design leans on M3/M1 over M2 is the M2a ceiling — which is the
explicit, priced cost of the angle, not an accident.

---

## 7. Single honest WEAKEST POINT

**Can a coarse signal — a static curated list + a one-bit activity gate, with *zero* per-process
cache measurement in the routing path — clear the M2a ≥50% agreement floor, and is M2a's ≥70%
target reachable at all?**

The design substitutes a **prior** (published winner names) for **measurement**, which is exactly
the objective's stated differentiator ("nobody on Windows routes by *measured* per-process cache
benefit"). Consequences, stated without softening:

1. **It structurally cannot discover an un-listed cache winner.** Any cache-sensitive process not
   in `kCuratedCacheWinners` is herded (if idle) or left (if active) — the router *guesses*. The
   contention literature is blunt that the real miss-counter beats every model and that "devils"
   (high-miss, low-reuse) are the *most* sensitive and defy name-based intuition
   [SOTA_CACHE_SCIENCE §3, V1]. A curated name list is precisely the heuristic that literature
   warns is a coarse pre-filter, not a verdict.
2. **M2a headroom is therefore capped.** Agreement is high on the curated set and on obvious
   idle/clock-bound cases, but every un-curated cache-sensitive process pulls agreement toward
   the **50% coin-toss floor**. On a §5 set dominated by curated + obvious cases, the floor clears
   and ~70% is reachable (especially with three-way WL3 scoring or the soft-CCD1-bias milestone);
   on a set salted with un-listed cache-sensitive work, **≥70% is unlikely and the floor itself
   is the honest risk.** If a live §5 measurement puts M2a below 50%, this candidate *fails on its
   own terms* — and the only remedy is to import the very per-process measurement this angle
   dropped, which would make it a different candidate.

In one sentence: **this design buys M1 (safety) and M3 (cost) with wide margin by paying in M2a
agreement headroom — it is the right architecture if the wins truly live in a small, knowable set
of cache-winners + broad herd isolation (which the prior art suggests), and the wrong one if the
objective's real value is discovering the long tail of un-listed per-process cache benefit, which
only measurement can find.**

<!-- Made with my soul - Swately <3 -->
