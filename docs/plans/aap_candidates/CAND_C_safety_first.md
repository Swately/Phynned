<!-- AAP DESIGNER artifact — one candidate architecture, written against the FROZEN OBJECTIVE -->
<!-- (docs/plans/PHYNNED_MASS_ROUTER_OBJECTIVE.md) in deliberate isolation from the other candidates. -->
<!-- Angle: SAFETY-FIRST — allowlist-and-reversibility AS the architecture. M1 (the veto) is the -->
<!-- organizing principle; efficacy is pursued only within what safety permits. -->

# Candidate C — Safety-First: Allowlist-and-Reversibility as the Architecture

**Author:** DESIGNER holon (Opus), AAP run 2, 2026-07-17.
**Angle:** optimize for **M1 (the quality veto)** as the organizing principle. Build a
conservative, **allowlist-gated, provably-reversible** router where *nothing is touched
unless it is on a positive allowlist of known-safe-to-move classes*, every action is
**write-ahead journaled** for crash-safe revert, and **hysteresis/cooldown** prevents
thrash and optimizer-fights. Efficacy is pursued strictly *inside* the safety envelope.

**The bet (stated plainly).** In this domain — an external, unsigned, elevated agent
mass-touching a live desktop — the dominant failure mode is *breaking something*, and the
winning product is the one that **never does**. Process Lasso, the most mature tool in this
space, *refuses* blanket all-process affinity: *"We do not recommend limiting the CPU
affinities of ALL processes… system processes should generally have unrestricted choice of
CPUs"* ([V1] F0, SOTA_SAFETY_COMPAT.md). This candidate takes that refusal as a design law,
not a caution. A smaller, safe, trustworthy router that clears the veto with margin and
delivers a *narrow but real* efficacy beats an aggressive one that occasionally crackles
audio or trips anti-cheat. The objective agrees on the ordering: `M1 = veto → M2 > M3 > M4`.

---

## 1. The architecture

### 1.1 The organizing inversion — default-DENY, positive allowlist

Today Phynned touches only what its `classify()` labels `Game`. MASS proposes to invert
that to "touch everything controllable." **This candidate rejects that inversion and keeps
the polarity: default is OBSERVE-ONLY (deny).** A process reaches a `Set*` call *only* by
passing a fixed admission gauntlet and matching a positive allowlist **class**. The
denylist is the *second* line of defence (E6 binding), never the first.

Every candidate process runs this gauntlet each time it is (re)classified. **Any step that
fails ends in `DoNotTouch` (observe-only), logged once with the reason (M4a):**

```
   ┌─ ADMISSION GAUNTLET (default terminal state = DoNotTouch) ──────────────────┐
   0. Denylist hit?              → DoNotTouch  (expanded table, §1.2)             │
   1. PPL / higher-integrity?    → DoNotTouch  (can't act anyway; never OpenProcess-SET) │
   2. AC descendant-tree hit?    → DoNotTouch  (walk PPID chain; never open SET, §2 M1d)  │
   3. Self-managed mask/prio?    → DoNotTouch  (non-default affinity at observe = its intent, F13/F18) │
   4. Launcher / generic host?   → DoNotTouch  (inheritance trap F10; opaque host F5)     │
   5. Foreground / audio-graph?  → DoNotTouch  (leave the game to AMD driver; audio to nobody) │
   6. Positive class match?      → if NONE → DoNotTouch. Else carry the class + its lever. │
   7. Coexistence manager active?→ DEFER  (AMD V-Cache optimizer / Process Lasso / Game Mode, E5) │
   8. → ADMITTED: journal-before-apply the class's sanctioned lever (§1.3).        │
   └────────────────────────────────────────────────────────────────────────────┘
```

Steps 0–5 are pure *exclusions* over the already-captured `NtQSI` snapshot (no extra
syscalls except one cheap `GetProcessAffinityMask` per survivor at step 3). Step 6 is the
only place a process can be *admitted*, and it can only be admitted into a class we shipped
or the operator opted into.

### 1.2 The allowlist classes (what is touchable, how membership is decided)

Membership is by **class**, each with an explicit predicate, a verification gate, and a
single sanctioned lever chosen as the *softest lever sufficient* for the goal:

| Class | Membership predicate | Verified by | Lever (softest sufficient) | Where it routes |
|---|---|---|---|---|
| **A — Named compute (curated)** | exact exe-name in `kAllowedComputeNames` (shipped, evidence-backed table) AND hw = X3D AND passed gauntlet | cache-science dossier (measured winners/losers) + on-box A/B before shipping a name | **hard affinity** (compute, non-AC, non-self-managed → safe to hard-pin) | winners→CCD0 (V-Cache), losers→CCD1 (clocks) |
| **B — Operator opt-in** | exe explicitly added by the operator via `ConfigStore` / UI | operator intent (F0: opt-in, measured, never blanket) | hard affinity or CPU-Sets (operator picks) | operator-declared target CCD, A/B-confirmed |
| **C — Safe-to-herd (soft pen)** | normal integrity AND default affinity AND non-foreground AND non-self-managed AND non-denylisted AND image path under user/Program Files (not `system32`) | the gauntlet itself + a positive "herdable" predicate | **CPU Sets** `SetProcessDefaultCpuSetMasks(CCD1)` — *soft*, power-aware, cannot starve, no child inheritance ([V1] control-plane §1.2) | penned to CCD1, V-Cache die kept clear for the game |

`kAllowedComputeNames` ships tiny and grows *only* by evidence: a name enters the table
after an on-box A/B run confirms a placement win (the same discipline `kGameNames` already
follows for the game-only tool). Seed entries, from the measured cache dossier:
`ShaderCompileWorker.exe` / UE5 shader-compile chain (**+16–19%** on V-Cache, [V2] Puget),
`rpcs3.exe` (PS3 emulation, reuse-heavy V-Cache winner, [V3]), `7z.exe` (**+~10%**, [V3]),
plus the *losers* routed the other way — ordinary `cl.exe`/`gcc`/`clang` code-compile
(**−5%** on V-Cache → CCD1, [V2/V3]). The winner/loser split is the point: *same tool
category, opposite answer, because the benefit is workload-intrinsic* (cache dossier §1.2).

Class C is a **later milestone** (M4, §5) and uses only the non-starving lever — it is the
mission's "pen the herd" element done in the one way that *cannot* cause the F12
oversubscription-crash class, because CPU Sets are advisory and the scheduler may always
overflow the set when cores are saturated ([V1] control-plane §1.2).

### 1.3 The expanded denylist (F0/F2 deltas — the second line of defence)

`observer/ProcessClassifier` today has `kSystemNames`
(`System, Registry, smss, csrss, wininit, winlogon, services, lsass, svchost, fontdrvhost,
dwm, explorer, SearchIndexer, WmiPrvSE, spoolsv, MsMpEng, SecurityHealth` + PID 0/4). It is
**correct for the game-only tool and has hard MASS gaps.** The deltas (SOTA_SAFETY_COMPAT.md
Facet-1 summary + ranked table):

- **Audio (F2, HIGH, audible):** `audiodg.exe` — never touched. Windows resets its affinity
  on service restart anyway, so a pin is *both* risky and non-durable.
- **Input/console (F4):** `ctfmon.exe`, `conhost.exe`.
- **Generic opaque hosts (F5):** `WUDFHost.exe`, `dllhost.exe`, `taskhostw.exe`, `sihost.exe`,
  `RuntimeBroker.exe`, `ShellExperienceHost.exe`, `StartMenuExperienceHost.exe`,
  `MemCompression` — their workload identity is unknown from the exe name, so a blanket
  policy cannot reason about them.
- **Virtualization (F6):** `vmmem`, `vmmemWSL`, `vmwp.exe`, `vmcompute.exe` — pinning a VM
  worker throttles the entire guest + every container inside it.
- **Third-party EDR (F7, HIGH — perf *and* tamper-detection):** name table
  (`CSFalconService.exe`, `SentinelAgent.exe`, Sophos, ESET…) **plus a path-based rule** —
  any image whose path is under a security-vendor directory is denied even an `OpenProcess`.
  The path rule catches EDR variants the name table misses.
- **Anti-cheat service table (F9/F11):** `EasyAntiCheat.exe`, `EasyAntiCheat_EOS.exe`,
  `BEService.exe`, `vgc.exe`, `vgtray.exe`, Vanguard host — never enumerated, never opened.
- **PPL runtime check (control-plane §4.2):** any process that returns
  `PROCESS_QUERY_LIMITED_INFORMATION` but denies `PROCESS_SET_INFORMATION` (or is flagged
  protected) is denied and cached as `DoNotTouch` — we can *see* PPL to skip it, never act.

The denylist is a **hard veto that no heuristic or override path can re-admit** — F1's
session-critical set (`csrss/wininit/winlogon/services/smss/lsass/System`) must never be
re-opened by any confidence rule or operator opt-in (Class B *cannot* override a denylist hit).

### 1.4 The journal-revert design (crash-safe M1b)

**The problem (F17, verified [V1]):** an affinity mask is a property *of the target
process*, not of the caller. If Phynned pins a process and is then `taskkill /f`'d, the pin
**persists indefinitely** — a C++ destructor does **not** run on `TerminateProcess`, so the
current RAII `~ActionExecutor() → revert_all()` guarantee holds only for graceful exit. MASS
leaves marks on many processes, so the leak is multiplied. **This is the K2 kill-criterion
and the M1b metric; it is this candidate's strong suit.**

**New module `action/RevertJournal`** — a write-ahead, flush-on-apply on-disk journal at
`%LOCALAPPDATA%\Phynned\revert.journal` (binary, fixed-size records; same trivially-copyable
discipline as `audit.bin`, but *write-through and fsync'd on apply*, not a lossy ring).

Record (per applied action):
```
{ magic, seq,
  exe_name[64], creation_time (FILETIME), image_path_hash,   // exe-IDENTITY, not raw PID
  pid,                                                        // hint only — PIDs recycle
  lever  (HardAffinity | CpuSet | Priority),
  prev_affinity_mask, prev_priority_class, prev_cpuset_state, // what to RESTORE
  applied_mask, apply_tsc,
  status (PENDING | APPLIED | REVERTED | ORPHANED_SKIP) }
```

**Write-ahead ordering (the crux — makes SIGKILL recoverable):**
1. Write record `status=PENDING`, **flush**.
2. Call the `Set*`.
3. Rewrite `status=APPLIED`, **flush**.
4. On clean revert → `status=REVERTED`, **flush**.

A kill anywhere in this sequence leaves a `PENDING`/`APPLIED` record that recovery treats
conservatively — it never assumes an unverified state.

**Recovery runs FIRST on agent start**, before ETW/observer/any new action (a new
`recover_journal()` step in `AgentRuntime::start()` ahead of the tick loop). This is *why*
it survives SIGKILL: recovery happens at the next boot of the agent, not in the dying
process. For each non-`REVERTED` record it reconciles against the live `NtQSI` snapshot by
**(exe-name + creation-time)**:

| Live state of the matched process | Action |
|---|---|
| current affinity == our `applied_mask` (we still own the pin) | revert to `prev_affinity_mask`; mark `REVERTED` |
| current affinity == `prev_*` already (OS/user restored it) | mark `REVERTED`, no `Set*` |
| PID gone (process exited) | mark `REVERTED` (mask died with the process) |
| PID recycled to a *different* process (creation-time mismatch) | **do not touch**; mark `ORPHANED_SKIP` + log |

**This also fixes the MASS bug the dossier calls out (F18/F13, `ActionExecutor.cpp:309–327`).**
Today `revert()` restores `default_affinity_mask_` (ALL cores) on purpose — to avoid
propagating a residual leak from a previously-crashed instance. That is *sound for games*
but **wrong for MASS**: it would *widen* any app that legitimately had a narrower
self-chosen mask. The journal makes revert-to-default unnecessary and wrong: it records the
*captured prev mask* and restores exactly that, and the gauntlet's self-managed skip (step 3)
means we never touched a self-managing app in the first place. So `revert()`/`revert_all()`
change to restore the journaled `prev_affinity_mask`; for Class-C herding, revert = clear the
process-default CPU set. The "residual leak from a prior crash" worry is now handled *by the
journal recovery itself* — the correct mechanism.

**Belt-and-suspenders — the dead-man's switch (F17 watchdog invert).** `core/InternalWatchdog`
already runs a background heartbeat/stall detector *inside* the agent. Add an *external* tiny
`phynned-revert.exe` that holds the journal path, watches the agent's liveness (a shared-memory
heartbeat word / named event), and — if the agent's heartbeat stops **and** the agent PID is
gone — replays the journal and reverts all. This covers the "killed and never restarted"
case that the on-restart recovery cannot (there is no restart). It is dormant ~100% of the
time (sleeps in chunks like the existing watchdog). *M1b's literal test is apply→`taskkill
/f`→restart, so the on-restart recovery is the primary mechanism; the dead-man is the
hardening for kill-without-restart.*

### 1.5 Module reuse / additions

**Reuse (the check-the-base rule, E3):**
- `observer/ProcessClassifier` — extend with the expanded denylist tables (§1.3), the
  `kAllowedComputeNames` allowlist table, self-managed-mask detection, and the AC
  descendant-tree walk. `TargetKind` gains `TouchableCompute` / `HerdPennable` / `DoNotTouch`.
- `action/ActionExecutor` — journal-back the apply/revert path (§1.4); add the CPU-Sets lever
  (`SetProcessDefaultCpuSetMasks`, [V1] control-plane, `PROCESS_SET_LIMITED_INFORMATION`,
  "cannot fail"); least-privilege handle opens (classify with
  `PROCESS_QUERY_LIMITED_INFORMATION`, act with `PROCESS_SET_INFORMATION` — never
  `PROCESS_ALL_ACCESS`, never `PROCESS_VM_READ` in the sweep, F19); RAII handle wrapper,
  `bInheritHandles=FALSE`, close immediately.
- `core/AutoRevertGuard` — reuse as the do-no-harm backstop (30s / 5s / 1.20× variance trip);
  **add a hang detector** (no-progress) alongside the variance detector (F12 amplifier).
- `core/InternalWatchdog` — reuse; add the external dead-man variant (§1.4).
- `core/SelfMonitor` — reuse unchanged for M3 accounting (already measures CPU%/RSS/handles).
- `learn/PerGameMemory` — reuse the bad-list: a process that trips the AutoRevertGuard is
  `mark_bad()`'d and never re-touched; reuse `LearnedEntry` + 30-day re-validation.
- `config/ConfigStore` — reuse for the Class-B operator opt-in list + denylist overrides.
- `bench/ABRunner` — reuse for M2a agreement, M2b improvement, M2c do-no-harm measurement.

**Additions:** `action/RevertJournal` (§1.4); `observer/Allowlist` (the gauntlet + class
predicates); `observer/CoexistenceDetector` (E5, §4); `policy/HysteresisGate` (the placement
sub-clock, §4); optional `phynned-revert.exe` (external dead-man).

---

## 2. How each M1 item is satisfied — with margin

**M1a — ≥4-h soak, zero allowlist violations, zero attributable crashes.**
Structurally, a `Set*` call is reachable *only* through the gauntlet, and the gauntlet's
only admitting step (6) records the class that admitted the process. A "violation" =
touching anything not admitted by a class — which is *impossible by construction* (there is
no other code path to `Set*`). The audit log (M4a) makes this auditable end-to-end. Margin
comes from three independent reducers of blast radius: (i) the touched set is *tiny and
curated* (named compute + operator opt-ins +, later, the soft herd) — far fewer chances to
break something than "touch everything except exceptions"; (ii) the softest-sufficient-lever
rule (CPU Sets for the herd *cannot starve*, [V1]); (iii) the self-managed + foreground + PPL
+ AC + launcher + host skips remove the highest-risk targets *before* any handle is opened.
*Exit (E8):* 4-h soak on the streamer mix (§5.4) → 0 admissions outside the allowlist in
`audit.bin`, and 0 WER/Reliability-Monitor crashes correlating (per the objective's
attribution method: audited action within the trailing 10 min) to a touched process.

**M1b — `taskkill /f` + restart reverts 100% of journaled placements.**
The write-ahead journal + `recover_journal()` on restart (§1.4) is the direct mechanism.
Margin: the two-phase `PENDING`/`APPLIED` status makes a kill *mid-`Set*`* recoverable; the
exe-identity key survives PID recycle (the documented failure mode of raw-PID keying, F17);
recovery runs *before* any new action so the box is clean before the agent does anything.
*Exit:* apply N placements → confirm masks persist after `taskkill /f` (proves the F17 gap is
real, not assumed) → restart → confirm 100% reverted to captured prev by pre/post mask
comparison. This is the **K2 go/no-go**; the external dead-man covers kill-without-restart.

**M1c — zero audio defects during the soak.**
`audiodg.exe` is denylisted (F2) — never touched, and Windows would reset a pin anyway.
Communication + streaming apps (`kCommNames`, `kStreamNames`) are excluded from herding and
**never receive EcoQoS** (F14: MS says EcoQoS "should not be used for performance-critical or
foreground user experiences… may cause instability"). Safety-first applies *no* lever to any
audio-graph, DAW, comms, or foreground process. *Instrument (objective M1c):* the ETW
audio-glitch counter (`Microsoft-Windows-Audio` glitches / `audiodg` GlitchCount) sampled
across the soak must not rise above its agent-off baseline; secondary = operator
live-listening during the game+OBS+Discord segment. **Margin:** we never touch the audio
stack, so our actions have *no mechanism* to move the counter.

**M1d — anti-cheat processes and their entire descendant trees: zero SET-rights handle opens.**
Gauntlet step 2 walks the **PPID ancestry chain** (from the already-captured `NtQSI`
snapshot — no syscall) before *any* `OpenProcess`-with-SET: if the process **or any ancestor**
matches the AC table or is a known AC-protected title, admission ends in `DoNotTouch` with
*never-open*. The inheritance trap is closed from the other direction too (F10): launchers
stay observe-only (step 4), because a pin on a launcher *inherits into* its AC-protected
child and can fail EAC's code-integrity check (the documented Elden Ring white-screen crash).
We also open the *minimum* right and never `PROCESS_VM_READ` in the sweep (F19). *Exit:* soak
with an EAC/BE/Vanguard title running → `audit.bin` shows **zero** `PROCESS_SET_INFORMATION`
opens against any exe in the AC table or its descendant tree.

---

## 3. Efficacy within the safety envelope (M2) — honest about the cap

**What routing it does.** On the *allowlisted set only*:
- **Class A (named compute):** route measured **V-Cache winners** (`ShaderCompileWorker`,
  `rpcs3`, `7z`) to **CCD0** by hard affinity — they are compute, non-AC, non-self-managed,
  so hard-pinning is safe and appropriate — and route measured **losers** (ordinary code
  compile, Cinebench-class) to **CCD1** for clocks. This is where **M2b** lives: reproduce
  the shader-compile **+16–19%** (cache dossier §1.2, [V2] Puget) by pinning
  `ShaderCompileWorker` → CCD0 and A/B-measuring cycles-per-unit-work (`QueryProcessCycleTime`,
  the objective's ground-truth instrument). One reproduced double-digit non-game win is
  *in hand* from the curated table.
- **Class C (later):** soft-pen the background herd onto CCD1 via CPU Sets, keeping the
  V-Cache die clear for the foreground game — which the **AMD 3D V-Cache Optimizer owns and
  we never touch**. This is the mission's "penned away from both" element, done with the
  non-starving lever.

**M2a — agreement %.** Because the allowlist is *curated from the cache dossier's measured
winners/losers*, agreement between the router's placement and the A/B-measured best placement
is **high on the processes it routes** — it should clear the ≥70% target and is nowhere near
the ≥50% floor *on the named set it acts on*. Measured by `ABRunner`: place on CCD0, measure;
place on CCD1, measure; the router agrees with the winner.

**M2c — do-no-harm.** This is safety-first's *strongest* M2 item. Any routed process that
regresses is caught by the `AutoRevertGuard` (30s window, 1.20× variance trip) *plus* the new
hang detector, auto-reverted, and `mark_bad()`'d so it is never re-touched. The herd literally
*cannot* regress from starvation because CPU Sets are advisory (the scheduler overflows the set
under load). So the "no workload regresses >2%" bound is met by *both* a measurement backstop
and a structural guarantee.

**The honest cap.** Safety-first delivers efficacy that is **real but narrow**. It clears
M2b (a curated double-digit win) and M2a/M2c *on the set it routes*, but it **declines to
route arbitrary/unknown processes by measured cache benefit** — the mission's actual novel
claim. It routes a curated list + soft-herds the rest, and grows the list only by evidence.
The *breadth* of M2 wins is capped by design. (This is the seed of the weakest point, §6.)

---

## 4. Cost budget (M3) + hysteresis/cooldown (F15) + coexistence (E5)

**The conservative approach is cheap — the arithmetic shows it.**
- **Steady state ≈ 0 `Set*` calls/tick.** Placement is **delta-driven** (control-plane §5):
  the gauntlet runs only on *newborn* PIDs (ETW birth event) and reclassifications, and a
  journaled "already-placed" flag prevents re-touch. The heavy `3N`-syscall cost of touching
  processes is paid **once per admitted process**, and safety-first admits *few*. Enumeration
  is the same in-repo bulk `NtQSI` (~80–150 µs at ~200 processes), unchanged.
- **Per-newborn gauntlet cost** = a handful of hashed string compares (denylist/allowlist,
  O(1)) + the PPID-ancestry walk *over the already-captured snapshot* (no syscall) + at most
  one `GetProcessAffinityMask` for survivors of step 3. Journal writes are small flushed
  appends **only on apply** (rare), never per-tick.
- **M3a (<0.5% of one core, 10-min avg):** met — steady state is observe-only; the expensive
  routing is one-shot per process. `SelfMonitor` already enforces and logs this (edge-triggered).
- **M3b (<20 MB RSS):** met — the new tables (allowlist/denylist/journal in-memory index) are
  a few KB; `PerGameMemory` is already ~48 KB fixed. No per-tick heap.
- **M3c (birth→placement p95 ≤500 ms):** met on the curated path — a named-compute match is an
  immediate decision on the ETW birth event; the gauntlet is cheap. (Honest note: if the
  *hysteresis dwell* is applied to an *uncertain* candidate we deliberately delay placement,
  but the curated set — which dominates the p95 — decides immediately, so p95 stays well
  under 500 ms. Every birth and `Set*` timestamp is logged, M4a.)

**Hysteresis / cooldown (F15 — against oscillation).** A **placement sub-clock slower than the
100 ms `AdaptiveTick`** (`policy/HysteresisGate`): two thresholds (engage/disengage) + a
per-PID **cooldown** so a process cannot be re-placed until stable for T seconds. For the
curated named set placement is **one-shot** (place once, done — no reactive loop, so
oscillation is *impossible*). For the optional reactive herd, ProBalance-style restraint
([V1] F15): act only under measured contention, prefer the reversible lever, exempt foreground
and self-managed.

**Coexistence (E5 / F16).** `observer/CoexistenceDetector` checks for the **AMD 3D V-Cache
Optimizer** service, **`ProcessGovernor.exe`** (Process Lasso's engine), and **Windows Game
Mode** being active. When any is managing a domain, **DEFER** — never fight. We ship **no
Forced-Mode equivalent** (F16: two continuous re-appliers = a flapping tug-of-war). The game
CCD is *always* the AMD driver's; if Process Lasso already set a mask on a PID, that PID reads
as non-default at observe time and is caught by the self-managed skip (step 3) regardless.

---

## 5. What it drops + staged milestones

**Dropped (deliberately, in service of the veto):**
- **Aggressive per-process routing of arbitrary/unknown processes** — the mission's broad
  "route *every* touchable process by measured cache benefit." We route the curated set +
  soft-herd; we do not measure-and-route every unknown process onto CCD0/CCD1.
- **Hard-affinity herding** — the herd uses soft CPU Sets (cannot starve → cannot cause the
  F12 oversubscription-crash class).
- **EcoQoS on any audio/comms/foreground** (F14).
- **Touching self-managed / PPL / AC / launcher / generic-host processes** (all skipped).
- **Job-Object background pens as a default** — the one-way "no un-assign a running process"
  trap ([V1] control-plane §1.3) conflicts with per-process reversibility; deferred to a
  careful later milestone or dropped in favor of revertible per-process CPU Sets.
- **Forced-Mode / continuous re-application** — never (F16).

**Milestones — each with a measurable on-box exit (E8/A4):**

| # | Scope | Measurable on-box exit |
|---|---|---|
| **M0** | Expanded denylist + gauntlet + least-privilege handles + write-ahead journal | Agent runs the gauntlet over the full ~300-proc table, touches **nothing** outside a 1-proc test allowlist; `audit.bin` shows 0 out-of-allowlist admissions; handle trace shows no `ALL_ACCESS`/`VM_READ` opens |
| **M1** | Journal recovery + revert-to-prev fix (F18) | apply → `taskkill /f` → masks persist (proves F17) → restart → **100% reverted to captured prev** (pre/post mask compare). **K2 / M1b go-no-go.** |
| **M2** | Class-A named-compute routing + `ABRunner` | `ShaderCompileWorker`→CCD0 A/B-measured **≥+10%** (cycles-per-unit-work) = **M2b**; ordinary compile→CCD1 with **no >2% regression** = **M2c** |
| **M3** | 4-h soak (streamer mix) + audio counter + AC-tree audit | **M1a** (0 violations, 0 attributable crashes) + **M1c** (audio counter ≤ baseline) + **M1d** (0 SET opens on AC tree). **The full veto, cleared with margin.** |
| **M4** *(growth)* | Class-C soft-herd (CPU Sets) + coexistence deferral + external dead-man | herd penned to CCD1 with **0 regressions** (CPU Sets can't starve, verified); deferral fires when AMD driver/PL/Game Mode present; dead-man reverts on kill-without-restart |

---

## 6. My single honest weakest point

**Safety-first may clear every M1 item and M2's floor while failing to be the *product the
objective describes* — it wins the veto partly by declining to play the game.** The mission's
differentiating claim is "*nobody on Windows routes by measured per-process cache benefit*."
This candidate, by refusing to touch arbitrary/unknown processes, reduces in practice to **a
curated placement list + a soft background pen** — which is *closer to what Process Lasso +
the AMD V-Cache driver already do* than to the objective's novel claim. M2b is safe (a curated
shader-compile win is in hand) and M2a is accurate *on the set it routes*, but the exposure is
**coverage, not accuracy**: if M2a's agreement % is scored over all five §5 workloads while
safety-first actively routes only ~2–3 of them (the named compute), the numerator is strong
but the breadth is thin — a reviewer could reasonably judge that the router's efficacy, though
real, is too narrow to justify the mass-router framing, and that the same safety could be had
by a much smaller tool. The mitigation (grow `kAllowedComputeNames` by evidence, add Class-C
coverage) is *exactly* the direction that reintroduces the risk the angle exists to avoid — so
the tension is structural, not incidental. **If this candidate loses, it loses on M2 breadth,
not on M1.**

<!-- Made with my soul - Swately <3 -->
