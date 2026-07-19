<!-- AAP A1 candidate — DESIGNER holon, angle: MEASUREMENT-FIRST / evidence-driven routing. -->
<!-- Written against the FROZEN objective (docs/plans/PHYNNED_MASS_ROUTER_OBJECTIVE.md), in -->
<!-- deliberate isolation from sibling candidates. Every cost/feasibility number carries its -->
<!-- basis; the honest weakest point is named in §6. -->

# CAND_B — Measurement-First / Evidence-Driven Router

**One-line thesis.** The differentiator is *measurement quality*, so make the measurement
engine the spine and let it *drive* placement: a router that **knows** which process gains
from the V-Cache CCD beats one that guesses, and that knowledge — per-exe, accumulated,
calibrated against an A/B ground truth — is worth more instrumentation cost (still under M3).
Placement at birth uses a **learned/provisional guess**; the measurement loop **corrects it
off the latency path** and grows the per-exe evidence base. This candidate optimizes **M2**
(efficacy: placement agrees with A/B-measured best) and **M4** (evidence) as the organizing
principle, holds **M1** as an inviolable veto, and defends the cost bet under **M3** with
burst — not always-on — instrumentation.

---

## 1. The architecture — a three-tier measurement engine that drives the router

The design is organized as a **measurement pipeline of increasing cost and increasing
authority**, feeding a placement decision. Cheap signals pre-filter; expensive signals only
run on the survivors; an A/B ground truth arbitrates and calibrates. This is the direct port
of the DIO/CAS heuristic (SOTA_CACHE_SCIENCE §4: "use the online LLC-miss-rate — or its A/B
cycle-count proxy — to decide cache-CCD vs frequency-CCD, per process, re-evaluate
periodically") adapted to Windows user/admin space, which the dossier calls the "unoccupied
niche" [SOTA_CACHE_SCIENCE finding #26].

### Tier 0 — always-on, near-zero: the behavioral pre-filter (who is even a candidate)

Runs every reconcile pass. Uses only cheap, near-unprivileged signals already in-tree:

- **Working-set trajectory** vs the **32 MB / 96 MB** L3 boundaries (`WorkingSetSize` from the
  existing FR-11 `ProcessMetricsSnapshot` bulk NtQSI capture). WS in the **32–96 MB band** =
  candidate V-Cache winner; WS ≪ 32 MB = clock-bound (never a candidate, route/leave on CCD1);
  WS ≫ 96 MB = likely bandwidth-bound (candidate only if reuse-heavy). Basis:
  SOTA_CACHE_SCIENCE §3 + finding #2, #18 (WS locates the process vs the boundary; page-fault
  rate is a *weak* proxy and is not used as a sensitivity signal).
- **`QueryProcessCycleTime`** (needs only `PROCESS_QUERY_LIMITED_INFORMATION`) → cycles/wall
  and cycles-per-unit-work when a work signal exists. Near-zero overhead [SOTA_CACHE_SCIENCE
  #17]. This is the *only* free per-process cycle instrument and doubles as the A/B metric
  (Tier 2).

Tier 0's job is **triage, not verdict**: it nominates a small candidate set (dozens at most)
from the allowlist. Everything it does not nominate is either penned (herd) or left alone —
it is never measured expensively. This is what keeps M3 affordable (§4).

### Tier 1 — burst PMC engine: the per-process cache-sensitivity estimate

For a nominated candidate, run a **bounded burst** of ETW **core-PMC counting on
`CSwitch`** — the one driver-free path to per-process hardware attribution
[SOTA_CACHE_SCIENCE §2.1, finding #10]. Counter delta between context switches is attributed
to the thread switched **out** → per-thread → per-process.

- **PMC sources selected** (from the §4.1 on-box listing; **max 6 simultaneous**, we use
  **4** to leave headroom for the IRQ/DPC "dirtying" noise and a fixed slot):
  `InstructionRetired`, `TotalCycles`, `CacheMisses`, `DcacheAccesses`. The derived signal is a
  **core-side MPKI approximation** = `CacheMisses / (InstructionRetired / 1000)`, plus CPI =
  `TotalCycles / InstructionRetired`. Basis: §4.1 confirms these four are live on the box.
- **What we do NOT claim:** true per-process L3-occupancy. It is physically impossible from
  software — L3-slice PMCs are per-CCX, Data-Fabric per-socket, both shared
  [SOTA_CACHE_SCIENCE finding #15]. Our MPKI is the *core-side misses this process issued*, an
  approximation, and **the level `CacheMisses` maps to on Zen 4 is unverified** (§4.1 caveat).
  Tier 1 is therefore an **accelerator**, never the load-bearing arbiter — Tier 2 is.
- **Burst, not always-on** (K3 compliance): a burst is a ~10 s window on **one candidate at a
  time**, serialized and rate-limited by `SelfMonitor`. CSwitch is the dominant ETW cost —
  tens-of-thousands to >100 k events/s [SOTA_EVENT_ENGINE #10] — so it is only paid during a
  burst, on demand, then the CSwitch/PMC session is stopped (the `SessionManager` start/stop
  hysteresis pattern already models this lifecycle).

### Tier 2 — A/B ground truth: cycles-per-unit-work (the arbiter and calibrator)

The **existing `bench::ABRunner`** (the A/B → apply → A/B state machine already in-tree) is
extended so its metric is **`ΔQueryProcessCycleTime / work_done`** — cycles-per-unit-work, the
*cleanest cross-CCD metric* because it removes the CCD0-clocks-lower confound that pollutes
wall-time [SOTA_CACHE_SCIENCE #23]. `work_done` = a per-process work signal (I/O bytes via
`GetProcessIoCounters`, frames, or an app-visible throughput log; generic proxy where nothing
better exists — this is a real limit, see §6).

Tier 2 does two jobs:

1. **Calibration** (resolves the §4.1 uncertainty, milestone M0 in §5): for each §5 workload,
   measure the Tier-1 PMC MPKI *and* the Tier-2 A/B verdict on CCD0 vs CCD1, and compute their
   agreement. **If PMC MPKI correlates with the A/B verdict** → PMC becomes a trusted fast
   predictor (cheap online routing). **If it does not** (the honest likely outcome given
   finding #15) → the router **falls back to Tier-2 A/B cycles-per-work as the sole signal**.
   Either way the routing signal is obtainable on the real box → **K4 satisfied by
   construction** (the A/B fallback always exists).
2. **Ground truth for M2a/M2c**: the A/B verdict *is* the "A/B-measured best placement" the
   objective scores against. It is also the do-no-harm instrument (§3).

### How it feeds the router + module reuse/additions

```
ETW birth (Kernel-Process)  ─┐
FR-11 NtQSI bulk snapshot   ─┤→ Tier 0 pre-filter → candidate set
QueryProcessCycleTime        ┘         │
                                       ▼
                         Tier 1 burst PMC (accelerator, if calibrated)
                                       │
                                       ▼
                         Tier 2 ABRunner cycles/work (arbiter)
                                       │
                                       ▼
              per-exe EVIDENCE (learn::PerGameMemory, extended)
                                       │
             ┌─────────────────────────┴───────────────────────┐
   birth-time provisional apply                    periodic re-measure + correct
   (CPU Sets, off latency path)                    (AutoRevertGuard do-no-harm)
```

**Reused as-is / lightly extended:**
- `bench::ABRunner` + `Baseline` + `DiffReport` — the A/B/A engine; swap the metric to
  cycles-per-work. This is the M2/M4 spine and it already exists.
- `observer::MetricsCollector` — its ETW ring, FR-11 bulk snapshot, TID→PID cache, and
  slim/fat PID-state split are exactly the substrate the tiers need. Add the PMC accumulator
  fields to the *hot candidate* PidState only.
- `action::ActionExecutor` + `AuditLog` — capture-prev/apply/revert + append-only audit. The
  journal (M1b) is an extension of `audit.bin`.
- `observer::ProcessClassifier` allow/deny tables; `learn::PerGameMemory` → the per-exe
  evidence store; `core::SelfMonitor`/`AdaptiveTick` for budget demotion; `topology::
  VCacheDetector` for the CCD masks; `phyriad::hw::{set_process_affinity, set_process_priority,
  set_thread_affinity}` (FR-3/FR-9/GFR-Ayama-2) for the hard-affinity override path.
- `phyriad::etw::SessionManager` — reused **but repaired and extended** (below).

**Net-new (this candidate owns building):**
1. **PMC-source configuration in `SessionManager`.** Today it only does
   `StartTraceA`+`EnableTraceEx2` and has **no PMC hookup** (verified: no `TraceSetInformation`
   / profile-source code anywhere in-tree). Add counting-mode PMC config
   (`TraceSetInformation` with the PMC counter list + `CSwitch` attach) — this is the concrete
   engineering that makes Tier 1 real.
2. **The three ETW repairs are prerequisite scoped work (E3), owned here:** F-1 wrong
   Kernel-Process GUID (`{0268a8b6-…}` → `{22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}`); F-2/F-3
   CSwitch needs `EVENT_TRACE_SYSTEM_LOGGER_MODE` + System Scheduler provider
   `{599a2a76-…}` keyword `CONTEXT_SWITCH` (the current session sets only
   `EVENT_TRACE_REAL_TIME_MODE` with StackWalkGuid — CSwitch is doubly broken); F-4 the ignored
   `EnableTraceEx2` return code (add per-provider status so failures can't hide again). Basis:
   SOTA_EVENT_ENGINE F-1…F-5.
3. **CPU-Sets wrapper** (`SetProcessDefaultCpuSetMasks`) and an **EcoQoS wrapper**
   (`SetProcessInformation` + `EXECUTION_SPEED`) — neither exists in `phyriad::hw` today (only
   affinity/priority/thread wrappers do). These are the soft, reversible, power-friendly levers
   the router prefers over hard affinity (§2, §3).
4. **The on-disk revert journal** (M1b), keyed on exe-identity+start-time.
5. **The calibration + evidence store** (Tier 2 verdict per exe, agreement bookkeeping for M2a).

---

## 2. M1 safety — the veto, treated first

Safety outranks everything (M1 = veto). Measurement widens *what we look at*; it must **not**
widen *what we act on*. The acting surface stays inside the allowlist.

- **M1 allowlist (E6, F0 — the primary control).** We act only on an **allowlist of touchable
  classes**: CPU-heavy productivity/compute exes and the documented V-Cache-winner classes
  (shader compilers, emulators, in-memory DBs — §5). The measurement engine may *observe* any
  process (read-only), but `apply()` is gated on allowlist membership. This is the F0 reframe:
  "allowlist of what may be touched," never "touch everything except exceptions." Process Lasso
  — the mature prior art — explicitly refuses blanket all-process affinity; we inherit that
  restraint.
- **M1 denylist (second line).** Extend `kSystemNames` with the SOTA_SAFETY_COMPAT deltas:
  `audiodg.exe, ctfmon.exe, conhost.exe, RuntimeBroker.exe, WUDFHost.exe, dllhost.exe,
  taskhostw.exe, sihost.exe, ShellExperienceHost.exe, StartMenuExperienceHost.exe, vmmem,
  vmmemWSL, vmwp.exe, vmcompute.exe, MemCompression` + an **EDR name/path table**
  (CrowdStrike/SentinelOne/Sophos/ESET) + an **anti-cheat service table**
  (`EasyAntiCheat.exe, EasyAntiCheat_EOS.exe, BEService.exe, vgc.exe, vgtray.exe`). These are
  **never `OpenProcess`-ed at all** — not even to read PMC (the safest handle is the one never
  requested; F19). PPL processes are skipped on `ACCESS_DENIED` and logged once (F2/F7/F19).
- **M1b journal-based revert (net-new — the SIGKILL gap).** Affinity/priority **persist after
  the setter dies**; a C++ destructor does **not** run on `taskkill /f` [SOTA_SAFETY_COMPAT
  F17, V1]. So RAII revert is insufficient by evidence. Mitigation: a small **on-disk journal**
  (extends `audit.bin`) records `{exe-identity, start-time, prev_mask, prev_priority,
  applied_mask}` **flushed on apply**. On agent start, **replay the journal and revert every
  still-live pin before doing anything else**, keyed on exe-identity+start-time (PIDs recycle;
  reconcile against the live process's *current* mask). This survives SIGKILL because recovery
  is at next agent start, not in the dying process. Directly satisfies M1b/K2.
- **M1c audio.** `audiodg.exe` is denylisted; the router **never routes any audio-graph, DAW,
  or communication process** (they are not on the allowlist, and EcoQoS is never applied to
  them — F14). Instrument: sample the ETW audio-glitch signal (Microsoft-Windows-Audio glitch
  events / audiodg `GlitchCount`) across the soak; a rise above agent-off baseline fails M1c
  (the objective's primary instrument), operator live-listen confirms.
- **M1d anti-cheat descendant trees.** The birth engine carries `ParentPID`; an
  AC-protected process **and its entire descendant tree** inherit a never-touch flag — we open
  **zero SET-rights handles** on them. Least privilege everywhere:
  `PROCESS_QUERY_LIMITED_INFORMATION` to classify, `PROCESS_SET_LIMITED_INFORMATION` to act via
  CPU Sets, `PROCESS_SET_INFORMATION` only for the hard-affinity override; **never**
  `PROCESS_VM_WRITE`, and `PROCESS_VM_READ` is dropped entirely from the mass sweep. The
  launcher-inheritance trap (F10: touching `steam.exe` broke EAC on the child) is avoided two
  ways: launchers stay observe-only (`kLauncherHelperNames` = Productivity), **and** CPU Sets
  do not inherit to children [SOTA_CONTROL_PLANE §1.2] — so even an accidental parent touch
  cannot propagate a mask into a protected child. Satisfies K1/M1d.

---

## 3. The routing decision function

### signal → CCD choice

For an **allowlisted candidate** (Tier 0 nominated it; WS in/near the 32–96 MB band):

```
sens = evidence.lookup(exe_identity)              // per-exe accumulated verdict
if sens is unknown:
    apply PROVISIONAL guess from class prior       // shader/emu/DB → CCD0; else CCD1
    schedule a Tier-1 burst + Tier-2 A/B           // correct later, off latency path
else:
    route to sens.best_ccd                          // CCD0 (V-Cache) or CCD1 (frequency)
```

- **V-Cache CCD0** iff the accumulated verdict says cycles-per-work on CCD0 < CCD1 **beyond the
  measurement noise band** (and WS ≤ ~96 MB with reuse — a large *streaming* WS like `lbm`
  is bandwidth-bound and must NOT be routed to CCD0 [finding #3]).
- **Frequency CCD1** for clock-bound work (WS ≪ 32 MB, or A/B shows CCD1 faster — e.g. ordinary
  code compilation, −5% on cache CCD [finding #7]).
- **Herd (non-candidate background)** → penned onto **CCD1** via a durable **Job Object**
  (`JOB_OBJECT_LIMIT_AFFINITY = CCD1 mask`, O(1) re-policy) or per-PID
  `SetProcessDefaultCpuSetMasks(CCD1)` — soft, reversible, no child inheritance,
  `PROCESS_SET_LIMITED_INFORMATION`, "cannot fail" [SOTA_CONTROL_PLANE §1.2/§1.3]. Game cores
  and herd cores are **disjoint by construction**, so the herd pen cannot contradict the game's
  placement (composability law: hard affinity silently beats CPU Sets — we never let them name
  the same cores).
- **Game (foreground)** → **untouched**; the AMD 3D V-Cache Optimizer + Game Bar own it (§7 of
  objective; commoditized). Coexistence handled in §5.

**Mechanism preference:** CPU Sets for individually-routed targets (soft, reversible, lighter
right, no child inheritance) [SOTA_CONTROL_PLANE §6]; hard affinity (FR-3) only as an override
when detection misses. Never shrink the mask of a process that already spawned wide (F12
oversubscription) or that holds a non-default self-chosen affinity (F13 — self-managing; leave
it).

### how M2a agreement is measured and reported

For each §5 workload, the router **logs its chosen CCD** and the **Tier-2 A/B-measured best
CCD**. **Agreement % = fraction of workloads where router-choice == A/B-best.** Reported in the
auto-generated per-app A/B report (M4b — the `bench` module extended). **Target ≥70%, floor
≥50%** (below floor the router is a coin toss → fails M2a). Because the router is *driven by*
the same A/B measurement it is scored against, agreement is high **by construction on the
benchmarkable set** — the risk is not agreement, it is whether the benchmarkable set
generalizes (see §6). Every decision also carries a logged human-readable reason (rule hit or
measurement values) → M4a end-to-end auditable.

### how M2c do-no-harm (≤2% regression) is enforced

Every commit is gated on a do-no-harm check: before a route is made permanent (or on a periodic
audit), run the Tier-2 A/B (cycles-per-work, **near-zero cost** — QPCT only, no PMC) comparing
the chosen placement vs **Windows-default placement**. **If the chosen placement regresses
>2%, revert immediately** and record "no benefit / harm" as a valid honest verdict (the
literature expects many processes to measure as no-benefit [SOTA_PRIOR_ART §8b.2]). This
extends the existing `core::AutoRevertGuard` (already a variance-regression trip, 30 s / 1.20×)
to a cycles-per-work regression trip. Satisfies M2c/K6 (per-decision revert path + regression
measurement both exist).

### the §4.1 LLC-mapping fallback (honest handling)

The design **does not depend** on `CacheMisses` mapping cleanly to LLC on Zen 4. Tier 1 PMC is
an *accelerator that must earn trust in calibration* (M0); the **load-bearing signal is always
the Tier-2 A/B cycles-per-work**, which is obtainable on the box regardless of PMC behavior.
If calibration shows PMC does not attribute cleanly, the router runs A/B-only — slower to
converge per exe, but correct. K4 cannot be hit.

---

## 4. Cost budget — closing under M3 (the instrumentation is the risk)

M3 ceilings: **<0.5 % of one core (10-min avg), <20 MB RSS, birth→placement p95 ≤500 ms.**
Instrumentation is where a measurement-first design can blow the budget, so the arithmetic:

### M3a — CPU < 0.5 % of one core (10-min average)

- **Tier 0 (always-on):** FR-11 NtQSI reconcile every **2–5 s**; ~0.3–0.6 ms @800 procs
  [SOTA_EVENT_ENGINE #11, DERIVED; base measured 80–150 µs @200 [V1-int]]. At one 0.5 ms pass /
  3 s = **~0.017 %** of one core. QPCT on the candidate hot-set (dozens) is sub-µs each →
  negligible. Birth engine (ETW Kernel-Process) is a **rare** event stream → near-zero.
- **Tier 1 (burst PMC) — the risk item.** CSwitch is the dominant cost [#10]. Bound it:
  bursts are **serialized (one candidate at a time)**, **~10 s each**, and rate-limited.
  Estimate the *in-burst* cost at **~2–3 % of one core** (CSwitch parse on a 32-thread box;
  this is the conservative end, since we filter to the one target's threads). Then:
  - one 10 s burst = ~0.25–0.3 core-seconds of work.
  - over a 600 s (10-min) window: **0.3 / 600 ≈ 0.05 %** average.
  - even **6 bursts / 10 min** (a lot for a stable desktop) = **~0.3 %** average — **under the
    0.5 % ceiling** with margin. Basis: in-burst % is an estimate flagged for on-box
    verification (SOTA per-syscall/per-event costs are `[V3]`/"MEASURE on the 7950X3D"); the
    *structural* claim — burst not always-on keeps the average tiny — is robust.
  - `SelfMonitor` **defers/aborts** a burst if the 10-min average approaches budget (budget
    breach reduces work, per its design). This makes M3a a *controlled* quantity, not a hope.
- **Do-no-harm A/B uses QPCT, not PMC** → cheap; it is not part of the PMC burst budget.

### M3b — RSS < 20 MB

- Slim process-level PID record ~48–56 B × ~1024 PIDs ≈ **~50–56 KB** [SOTA_EVENT_ENGINE #12/
  §3.3, DERIVED]. Fat per-thread state (~1 KB) only for the **small actively-routed/candidate
  set** (dozens) → ~tens of KB. PMC accumulators live only on the hot candidate. Per-exe
  evidence store: hundreds of exes × tens of bytes → sub-MB. ETW rings pre-allocated. The
  agent carries **no UI/Vulkan deps** (UI is a separate process over SHM). Target **< 20 MB**
  is met with wide margin; the bar (Process Lasso core engine 1–3 MB [#17]) is the aspiration.
  Prerequisite: widen `pid_hash` `slot_plus_1` `uint8_t`→`uint16_t` and `kMaxTargets` 64→1024+
  (the ~254-PID ceiling is a hard cap today, F-5).

### M3c — birth→placement p95 ≤ 500 ms

- **Placement at birth uses the provisional per-exe guess — it does not wait for
  measurement.** So the latency path is: ETW ProcessStart delivered → classify (in-event
  fields: PID/ImageName/ParentPID) → `OpenProcess`+CPU-Set apply. That apply is a few ms
  [SOTA_CONTROL_PLANE §5; SOTA_EVENT_ENGINE §4.1]. The measurement (Tier 1/2) runs *after*,
  off the latency path, and corrects the guess.
- **The honest risk:** a lone low-volume birth session flushes on the FlushTimer (**min 1 s**)
  [SOTA_EVENT_ENGINE #9/§2.4], which would blow p95. Mitigation: pair the birth stream's
  buffers with a low-volume secondary so they fill/flush in ms, or accept that sub-second birth
  routing is the lever that must be tuned on-box. This is the same constraint every candidate
  faces; naming it is the honest move (see §6 note). With the provisional-guess design the
  *decision* is instant — the residual is event-delivery latency, not measurement latency.

---

## 5. What it drops, coexistence, staged milestones

### Dropped (deliberate scope narrowing)

- **Does NOT measure the whole process table.** Only allowlisted candidates get Tier-1/Tier-2
  measurement. The herd is **penned by a cheap static rule**, never measured — this is what
  makes M3 affordable and is honest about where measurement pays.
- **Drops always-on CSwitch/PMC** (K3) — burst only.
- **Drops game-layer routing** — AMD owns it (§7).
- **Drops true per-process L3 occupancy** — physically impossible (finding #15); uses core-side
  approximation + A/B arbiter.
- **Drops the "+98 %-headline" provisional numbers** the prior-art dossier flags as unvalidated
  [SOTA_PRIOR_ART §8b.1] — this candidate reports only what its own A/B measures.

### Coexistence (E5)

Detect and defer to: the **AMD 3D V-Cache Optimizer service** (`amd3dvcacheSvc.exe`), **Process
Lasso** (`ProcessGovernor.exe`), and **Windows Game Mode**. On detection, **defer/bound** our
actions on the contested target to avoid policy flapping (two continuous re-appliers = a
tug-of-war, F16). **Ship no Forced-Mode equivalent on by default.** The herd pen (disjoint
cores) cannot fight the game layer by construction.

### Staged milestones (each with a measurable on-box exit — E8/A4)

| # | Milestone | Measurable on-box exit |
|---|---|---|
| **M0** | **Calibration — resolves §4.1** | For the §5 workloads, measure Tier-1 PMC MPKI *and* Tier-2 A/B cycles-per-work on CCD0 vs CCD1; compute agreement. **Exit GREEN either way:** PMC correlates → adopt as accelerator; PMC does not → adopt A/B-only. The §4.1 LLC-mapping uncertainty is *closed* (this is the milestone that de-risks the whole PMC bet). |
| **M1** | **ETW repair + PMC config** | Fix F-1…F-4; add PMC-source config to `SessionManager`. Exit: birth events delivered (count > 0 via `logman`/return-code check), CSwitch delivered, **PMC counters non-zero on CSwitch** on the box. |
| **M2** | **Safety** | Allowlist+denylist+journal-revert+audio wired. Exit: **M1a/b/c/d all green** on the ≥4 h live-desktop soak (zero allowlist violations, `taskkill /f`→100 % journal revert, audio-glitch counter flat, zero AC SET-handle opens). |
| **M3** | **Efficacy** | Run the §5 set. Exit: **M2a agreement ≥ floor (≥50 %, target ≥70 %)** reported as a %, **M2b ≥1 reproduced double-digit non-game win** (shader-compile is the documented ~16–19 % candidate), **M2c no workload regresses >2 %**. |
| **M4** | **Cost** | On the idle-flood (~300 procs) and streamer-mix cases: **M3a <0.5 %** (10-min SelfMonitor avg incl. bursts), **M3b <20 MB RSS**, **M3c p95 ≤500 ms** birth→apply (logged). |

Ordering respects the veto: M2 (safety) gates before M3/M4 are claimed; M0 gates the PMC
investment before M1 builds it.

---

## 6. Single honest WEAKEST POINT

**The measurement-quality bet is strongest exactly where evidence is easy, and weakest exactly
where the "mass" ambition lives — and the design papers over that gap with the allowlist.**

Concretely: the Tier-2 A/B arbiter (the load-bearing signal, since Tier-1 PMC will *probably*
fail to attribute cleanly on Zen 4 — finding #15 says core-side misses are all we get) needs a
per-process **unit-of-work** signal to make cycles-per-work meaningful. For the benchmarkable
allowlist — shader compilers, emulators, databases — a work signal exists (batch time, frames,
transactions) and the router will measure well and agree with ground truth. But for the
**opaque herd** — the hundreds of background processes that are the actual "mass" in
"mass-router" — there is **no honest work signal**; `GetProcessIoCounters` bytes are a poor
proxy (I/O bytes ≠ useful forward progress). So "route by *measured* benefit" is really "route
by measured benefit **for the handful of allowlisted, work-signal-having candidates**," while
the herd is placed by a **static** pen with **no measurement at all**.

That means the candidate's headline differentiator (measurement quality) delivers on a much
**narrower** set than the objective's "every touchable process gets placed on the cores it
benefits from" implies. It is not wrong — the narrow set is real, defensible, and is precisely
the unoccupied niche [SOTA_PRIOR_ART §8b.1/8b.2] — but the *measurement-first framing
over-promises breadth*. If the operator's true want is breadth (mass), this design's honest
reach is the benchmarkable minority; if the want is a defensible *evidence* niche, this design
hits it squarely. The weakest point is that tension, and I will not hide it behind the
agreement % (which is high *because* it is scored only on the set the router can actually
measure).

<!-- Made with my soul - Swately <3 -->
