<!-- AAP A1 candidate — DESIGNER holon, angle A: EVENT-DRIVEN-FIRST / birth-time placement. -->
<!-- Written against the FROZEN OBJECTIVE (docs/plans/PHYNNED_MASS_ROUTER_OBJECTIVE.md), -->
<!-- in isolation from sibling candidates. Every cost/feasibility number carries its basis. -->

# CAND_A — Event-Driven-First / Birth-Time Placement

**Angle.** Optimize for M3c (birth→placement latency) and M3a/b (near-zero cost) as the
*organizing principle*. The architecture is built around the corrected ETW
Microsoft-Windows-Kernel-Process **birth notifier**: a process is placed the instant it is
born, from cheap birth-time signals (image name, ParentPID, an O(1) rule/learned-decision
lookup). Measurement is **deferred and rare**; polling is demoted to a slow janitor.

**The bet, stated plainly.** Most of the value is captured by a *fast, correct, cheap*
birth-time router backed by a rule + learned-decision cache. Heavy per-process measurement
is the *exception* (a rare A/B run for a not-yet-known exe), not the steady state. If that
bet holds, the M3 ceilings are met with an order of magnitude of headroom because the agent
spends steady-state life **blocked on the ETW consumer thread**, waking only on births.

---

## 0. Why this shape, in one breath

The event dossier's decisive finding [V1]: **ETW Microsoft-Windows-Kernel-Process
ProcessStart (Event ID 1) already carries `PID`, `CreateTime`, `ParentPID`, `SessionID`,
`ImageName` at spawn — enough to route at birth with no follow-up query.** So the routing
*decision* costs a table lookup, and the routing *apply* costs one `OpenProcess`+`Set`+
`CloseHandle` on exactly one PID. There is no per-tick scan of the process table in the hot
path. That is the whole cost argument (§4), and it is why this angle can afford to spend its
scarce cycles on *safety* and *deferred measurement* instead of on detection.

---

## 1. Architecture

### 1.1 Modules — reuse, extend, add

**Reuse as-is:**
- `framework/topology/VCacheDetector` — `vcache_logical_ids()` gives the real CCD0 (V-Cache)
  core set at runtime; never hardcode masks.
- `learn/PerGameMemory` — the `(exe, hardware_id) → LearnedEntry` table and the `BadEntry`
  regression blacklist are exactly the birth-time **learned-decision cache** this angle
  needs; `find(exe)` is the O(1) lookup on the hot path. `is_bad(exe)` gates re-touch.
- `core/SelfMonitor` — the M3a/M3b instrument (already in-tree); its budget-breach
  edge-trigger becomes the self-demotion signal.
- `action/AuditLog` (`AuditRecord`, 96 B) — the M4a per-decision reason trail and the seed
  of the M1b journal.
- `config/DefaultPolicyPack` + `policy/Rule` — the class→action templates
  (`PinGameToVCacheCcd`, `EvictStreamFromHotCcd`, …) become the birth-time rule table.

**Extend:**
- `framework/etw/SessionManager` — **fix the four defects first** (E3, scoped work this
  candidate owns; see §6-M0): the `kKernelProcess` GUID at `SessionManager.hpp:159-161` is
  `{0268a8b6-…}` and must become `{22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}` [V1, event F-1];
  `kKernelContextSwitch` is actually StackWalkGuid [V1, F-2]; CSwitch needs
  `EVENT_TRACE_SYSTEM_LOGGER_MODE` not the current `EVENT_TRACE_REAL_TIME_MODE` [V1, F-3];
  `EnableTraceEx2`'s return code is ignored [V1, F-4]. Add a per-provider status check so
  these can never hide again.
- `observer/ProcessClassifier` — extend the name tables into the **allowlist/denylist** of
  §2; add the MASS denylist deltas (audiodg, ctfmon, conhost, RuntimeBroker, WUDFHost,
  dllhost, taskhostw, sihost, ShellExperienceHost, StartMenuExperienceHost, vmmem/vmmemWSL/
  vmwp/vmcompute, MemCompression, an EDR name/path table, an AC-service table). Keep
  `check_d3d_vk_modules` but **drop it from the birth hot path** — birth routing is by name
  + parent, not by module inspection (VM_READ is a detection red flag, safety F19).
- `action/ActionExecutor` — add a CPU-Sets apply path (`SetProcessDefaultCpuSetMasks`, the
  lighter `PROCESS_SET_LIMITED_INFORMATION` right, "cannot fail" [V1 control-plane §1.2]) and
  an EcoQoS path (`SetProcessInformation` + `EXECUTION_SPEED`); change revert to restore the
  **captured prior**, not the system default, for self-managed apps (safety F13/F18). Widen
  `kMaxActiveActions` and the `pid_hash` slot from `uint8_t` (≤254 PIDs) to `uint16_t`
  (event F-5 / #13 — hard prerequisite for MASS).
- `core/AgentRuntime` — replace the poll-first loop with the birth→decision→apply data path
  (§1.2); demote the full scan to a 2–5 s reconcile janitor.

**Add (new modules):**
- `observer/BirthRouter` — the consumer-thread callback: parse ProcessStart → denylist gate
  → allowlist classify → cache/rule lookup → enqueue a `PlacementIntent`. Lightweight, copy-
  and-return (SessionManager callback contract).
- `observer/ProcessTree` — an incremental parent→child map built from `ParentPID` at each
  birth (free from the event). Powers M1d descendant-tree AC avoidance and launcher-
  inheritance safety (F10) at zero query cost.
- `core/PlacementJournal` — the on-disk crash-durable revert journal (M1b / K2; §2).
- `learn/AbRefiner` — the deferred, one-at-a-time A/B measurement worker (§3); owns the
  on-demand Session B (CSwitch/PMC), started only during an active A/B.
- `observer/BirthLatencyProbe` — optional WNF create-signal subscriber that force-flushes
  Session A (§1.3, the M3c latency lever).

### 1.2 The birth → decision → apply data path

```
ETW Session A (Kernel-Process {22FB2CD6-…}, kw 0x10)  ── consumer thread (blocks in ProcessTrace)
   │  ProcessStart(PID, CreateTime, ParentPID, ImageName)   [V1]
   ▼
BirthRouter callback  (copy fields, return — no OpenProcess here)
   ├─ ProcessTree.insert(PID, ParentPID)                    // free tree build
   ├─ DENYLIST gate  → hit? → observe-only, log, DONE       // §2, second line of defence
   ├─ AC-tree gate   → PID or any ancestor AC-protected? → NEVER open SET handle, DONE  // M1d
   ├─ ALLOWLIST classify (image name → class)               // §2, first line of defence
   │     └─ not on allowlist? → observe-only, DONE
   ├─ CACHE lookup  PerGameMemory.find(exe)                 // O(1), the learned decision
   │     ├─ hit + fresh + not bad → PlacementIntent(cached mask/mechanism, reason="learned")
   │     └─ miss/stale → rule default for class             // reason="rule:<name>"
   └─ enqueue PlacementIntent → dirty set
                                          │
   Apply pass (drains dirty set, coalesced):
   OpenProcess(min rights) → Set{Affinity|CpuSetMask|EcoQoS} → CloseHandle
   → PlacementJournal.append(exe-identity, start-time, prev, applied)  BEFORE clearing dirty
   → AuditLog.write(APPLY, reason)                          // M4a
```

Steady state = the consumer thread asleep inside `ProcessTrace()`. A birth wakes it; the
apply pass touches exactly one process. A **reconcile janitor** runs `NtQSI` every 2–5 s to
catch missed births / exits / PID recycle and to prune dead journal/action entries.

### 1.3 Control mechanism per layer, and WHY (the composability law)

The binding law [V1 control-plane §1.2(2)]: **a restrictive hard affinity mask is respected
above any conflicting CPU-Set assignment** — hard affinity silently wins. And **hard
affinity inherits to children; CPU Sets do not** [V1]. The four layers are chosen so that
the *only* hard-affinity mask in the system names CCD0 and the soft CPU-Sets name CCD1 →
**the two are core-disjoint by construction and cannot fight** (E4).

| Layer | Population | Mechanism | Why (composability + safety) |
|---|---|---|---|
| **Game** (foreground, ≤ few) | detected game | **NONE — observe-only** (optional hard-affinity override only on explicit operator request) | AMD 3D V-Cache Optimizer + Game Bar already own game→cache-CCD routing [V3]; objective §7 concedes it. Touching the game (or its launcher) risks the anti-cheat inheritance crash (F10 — affinity inherits, broke EAC on Elden Ring). Kill criterion K1 forbids SET handles on AC trees. So the safest game action is *no action*. |
| **Cache-routed** (measured V-Cache winners: shader-compile, emulator, in-mem DB) | allowlisted, measured or rule-primed | **hard affinity → CCD0** (`SetProcessAffinityMask`) | We *want* inheritance here: a shader-compile launcher's worker children should also land on V-Cache — hard affinity gives that for free [V1]. We *want* it to beat any CPU-Set the herd sets. Positive concentration onto 8C/16T (not a 2-core squeeze) keeps F12 oversubscription risk low. Reversible (re-apply full mask), journaled. Skip if the process already self-set a narrower mask (F13). |
| **Clock-routed** (measured clock winners: ordinary compile, video encode) | allowlisted, measured | **CPU Sets → CCD1** (`SetProcessDefaultCpuSetMasks`) | A *preference*, not a hard pin — if CCD1 saturates the scheduler may spill, which is what we want for throughput work; no child inheritance to fight; lighter right; "cannot fail" [V1]. Disjoint from CCD0 hard affinity. |
| **Background herd** (hundreds; the ~300 idle flood) | allowlisted herd classes | **CPU Sets → CCD1** (+ optional **EcoQoS** for genuine churners) | Soft, power-manager-friendly, reversible, doesn't infect children, "cannot fail" [V1]. **Not a Job Object**, despite its O(1) re-policy appeal [V1 §1.3]: job membership is one-way / non-reversible per process, which fails the M1b journal-revert requirement (K2). Event-driven births are one-at-a-time, so the herd's O(N) per-process apply cost is **O(1) per birth event** — the Job Object's batch advantage only matters for whole-table sweeps, which this angle drops. EcoQoS never on audio/comm/foreground (F14). |

Herd-CCD1 (soft) and clock-CCD1 (soft) share cores harmlessly (both say "you may use CCD1").
Cache-CCD0 (hard) never names CCD1, so it can never silently override the herd's CPU Sets.
The composability law is satisfied by geometry, not by hope.

---

## 2. How each M1 safety item is satisfied (the veto — treated first)

Safety is the QUALITY VETO; a design failing any M1 item is dead (objective §2). The
allowlist reframe (E6 / dossier F0 — Process Lasso itself refuses blanket all-process
affinity) is the load-bearing decision: **the mass layers act on an allowlist of touchable
classes; the denylist is the second line of defence, never the first.**

- **M1a (zero allowlist violations, zero attributable crashes over a 4 h soak).**
  *Allowlist model:* at birth, image name → class; only classes on the allowlist (background
  herd, known cache-winner exes, known clock-winner exes) are ever touched. Unknown → observe
  only. *Denylist* (second line): the expanded table (§1.1) is checked **before** any
  `OpenProcess` — a hit means never open a SET handle. *Crash attribution* is exactly the
  objective's method: cross-reference WER/Reliability-Monitor failures against
  `audit.bin`/journal timestamps in the trailing 10 min; the journal already records
  (exe-identity, start-time, prev, applied) per apply, so attribution is a timestamp join.
  *Oversubscription guard* (F12): never shrink the mask of a process whose observed
  `thread_count ≫` target cores; cache-routing concentrates (grows onto 16T), it does not
  squeeze.

- **M1b (`taskkill /f` + restart reverts 100% of journaled placements).**
  `core/PlacementJournal`: on every apply, **before** the dirty entry is cleared, append
  `(exe-identity, start-time, prev_affinity, prev_priority, prev_cpuset_state, mechanism)` to
  an on-disk journal and `fflush`. Affinity/CPU-Sets **persist after the setter dies** [V1,
  F17] and a C++ destructor does **not** run on `TerminateProcess` — so RAII revert is
  insufficient (the objective says so explicitly). Recovery is at **next agent start**: replay
  the journal, and for each entry whose target is still live (matched by exe-identity +
  start-time, since raw PID recycles), revert to the **captured prior** (F13/F18 — restore
  the app's own prior mask, not the blunt system default). This survives SIGKILL because
  recovery lives outside the dying process (K2 satisfied). Optional dead-man watchdog (F17)
  as belt-and-suspenders.

- **M1c (zero audio defects; instrument = the ETW audio-glitch counter).**
  `audiodg.exe` is on the denylist → never touched (folk-remedy pinning of it causes xruns
  and Windows resets it anyway, F2). EcoQoS is **never** applied to audio/comm/foreground
  classes (F14 — MS: "may cause instability … not for perf-critical"). Positively, the agent
  *subscribes* to the M1c instrument itself — Microsoft-Windows-Audio glitch events /
  audiodg GlitchCount — as a cheap safety monitor: any rise above the agent-off baseline
  triggers immediate self-back-off of the herd/EcoQoS layer. Because we never touch the
  audio graph, the expected counter delta is zero; the subscription makes that *measured*,
  not asserted.

- **M1d (zero SET-rights handle opens on AC-protected processes or their descendant trees).**
  This is where birth-time wins for free: the ProcessStart event **gives `ParentPID`**, so
  `observer/ProcessTree` builds the ancestry map incrementally at birth with **zero extra
  queries**. Any process whose image matches the AC table (EasyAntiCheat, BEService, vgc,
  Vanguard-protected titles) OR whose ancestor chain includes one → the entire descendant
  subtree is flagged **never-open** at birth. Launchers stay observe-only
  (`kLauncherHelperNames` → Productivity), so we never set an affinity a game could inherit
  (F10). AC handles are silently stripped of SET rights by the kernel callback anyway [V2],
  but the point is we never *request* one (K1 satisfied by never opening, not by handling
  the denial).

---

## 3. Routing signal at birth, and how measurement refines it (M2a)

**Birth signal (the cheap, immediate decision):**
1. `ImageName` → class (allowlist name tables). 
2. `ParentPID` → inherit parent's tree policy / AC flag (ProcessTree).
3. `PerGameMemory.find(exe)` → the **learned decision** (a mask + mechanism validated by a
   prior A/B on this hardware_id). Cache hit + fresh + not on the bad-list ⇒ apply it.
   Cache miss/stale ⇒ apply the **class rule default** (cache-winner → CCD0 hard affinity;
   clock-winner → CCD1 CPU-Set; herd → CCD1 CPU-Set) as a *prior*, and mark the exe for a
   deferred A/B.

**Deferred, rare refinement (the measurement, demoted to the exception):**
- `learn/AbRefiner` runs **one A/B at a time**, only for an allowlisted exe that (a) lives
  long enough to measure and (b) has no fresh learned entry. Metric = **cycles-per-unit-work**
  = `ΔQueryProcessCycleTime / work_done` [V1 cache §4] — cycles removes the CCD0-clocks-lower
  confound that pollutes wall-time. Place on CCD0, measure; place on CCD1, measure; the winner
  updates `PerGameMemory` (so the *next* birth of that exe is an instant cache hit). This is
  the ground-truth arbiter the objective mandates because the LLC-level PMC mapping on Zen 4
  is unverified (§4.1 / cache #12).
- **Session B** (on-demand CSwitch/PMC, `EVENT_TRACE_SYSTEM_LOGGER_MODE` + System Scheduler
  `{599a2a76-…}`, 64–128 KB buffers [V1]) starts **only** during an active A/B and stops
  after — satisfying K3 (no always-on full-rate tracing). PMC counters (`CacheMisses`,
  `InstructionRetired`, `TotalCycles`; max 6 [V1 §4.1]) are a *secondary* refinement gated
  behind the on-box `wpr -pmcsources` calibration; the A/B cycle metric is the arbiter if PMC
  LLC mapping proves unusable.

**M2a agreement, measured (M4a/M4b):** every birth logs its decision + reason; every A/B
logs the measured winner and whether it **agreed** with the birth decision. Agreement % =
(births whose cached/rule decision matched the later A/B winner) ÷ (births later measured).
Report against the ≥70 % target / ≥50 % floor. M2b (≥1 double-digit non-game win) is the
shader-compile A/B (documented ~16–19 % V-Cache winner [V2]); M2c (do-no-harm ≤2 %) is the
same A/B instrument run against Windows-default placement.

---

## 4. Cost budget — closes under M3's hard ceilings (with arithmetic + basis)

Convention: "% of one core." 7950X3D = 32 threads, so 0.5 % of one core = ~0.016 % of the box.

### M3a — steady-state CPU < 0.5 % of one core (10-min avg; SelfMonitor)

| Component | Per-event cost (basis) | Rate | CPU |
|---|---|---|---|
| Birth parse + denylist/allowlist/cache lookup + 1× Open/Set/Close | ~10 µs (Open/Set/Close single-digit µs [V2 event §4.1, control §5]; lookup ~hundreds ns) | 20 births/s sustained (busy desktop; idle ≪1/s) | **0.02 %** |
| Reconcile janitor (full NtQSI) | ~0.3–0.6 ms @800 procs ([V1-int] 80–150 µs @200 + [DERIVED] linear scaling, event #11) | every 3 s | **0.02 %** |
| Birth-latency lever (WNF callback → ControlTrace FLUSH) | ~µs per publish | ~births rate | **~0 %** |
| **Total sustained** | | | **≈ 0.05 %** |

Even a coalesced launcher storm (200 births/s × 10 µs = 2 ms/s = 0.2 %) is momentary, not
sustained. **Headroom to the 0.5 % ceiling is ~10×.** Crucially, this angle **drops the
100 ms full-table Active-tick scan** (the source of the historically-measured sustained
~2.9 % during Fallout 4 [V1-int]) — births are event-driven, so there is no per-tick scan in
the hot path. *Honest caveat:* the 0.05 % is mechanism-derived; the M3a exit is the on-box
SelfMonitor number on workload 5 (§6-M2).

### M3b — agent RSS < 20 MB (UI excluded)

- Slim per-PID state **48–56 B** (drop per-thread hot-TID array; [V1/DERIVED event #12/§3.3])
  → 1024 PIDs ≈ **~50 KB**. Fat 1 KB/PID state only for the ≤ few processes in an active A/B.
- Learned-decision cache (`PerGameMemory`) 256 × 192 B ≈ **48 KB** (already in-tree).
- ProcessTree + journal ring: a few tens of KB.
- Per-PID state is **not** the 20 MB driver — CRT + framework is. The resident agent carries
  **no Vulkan/UI** (UI is a separate process over SHM, §5.1 event dossier); the bar is
  Process Lasso's engine at ~1–3 MB [V2]. Target < 20 MB is comfortable; exit = on-box RSS.
- Prerequisite: widen `pid_hash` slot `uint8_t`→`uint16_t`, `kMaxTargets` 64→1024+ (event
  F-5); trivial in bytes given slim state.

### M3c — birth→placement p95 ≤ 500 ms (ETW birth ts → Set* ts)

The honest ETW fact [V1 event §2.4]: a **lone** birth session flushes on buffer-full **or**
the FlushTimer (**min 1 s**), so an idle-desktop lone birth can wait ~1 s — which alone would
blow p95. The latency lever (within envelope; no driver, K5):
- **WNF create-signal → `ControlTrace(EVENT_TRACE_CONTROL_FLUSH)`.** The WNF process-creation
  state name fires ~immediately, no admin, negligible cost [V3 event #18]. Used **only** as a
  *flush trigger* (never as the source of truth — that stays ETW ProcessStart [V1]): on WNF
  publish, force-flush Session A → the ProcessStart event lands in a few ms instead of waiting
  for the 1 s timer. Then Open/Set is single-digit µs. **Typical birth→placement: a few ms.**
- Under the active/streamer workloads births cluster and image-load traffic keeps buffers
  filling, so delivery is sub-500 ms without WNF; p95 is comfortably met.
- **Residual tail (named honestly):** a truly idle desktop lone birth with WNF unavailable
  (undocumented, version-fragile) degrades to the ~1 s flush-timer path. This is the M3c risk;
  the WNF lever plus the natural buffer-fill of any real activity keep p95 under 500 ms for
  the workloads that matter, and the objective's own §1 concedes the user-mode floor is "a few
  ms *after* first schedule." Exit = measured birth→placement distribution on the real box.

---

## 5. What it drops, coexistence, milestones

### 5.1 Dropped vs the current design
- **The 100 ms full-table scan as the primary detector** → demoted to a 2–5 s reconcile
  janitor (event §3.4). This is the single biggest cost + latency win.
- **Per-thread hot-TID tracking** (1 KB→48 B/PID) for all but the tiny measured set (#12).
- **Always-on measurement** → on-demand Session B, gated behind an active A/B only (K3).
- **Game-pinning as the flagship** → game layer is observe-only, deferring to AMD's driver
  (objective §7; prior-art §8a — commoditised).
- **"Touch everything"** → allowlist-first (F0/E6).
- **The four broken ETW GUIDs/modes** → fixed as scoped M0 work (F-1…F-4, E3).
- **`check_d3d_vk_modules` on the hot path** (VM_READ detection flag, F19) → birth routes by
  name + parent only.

### 5.2 Coexistence (E5)
Detect at start and on reconcile: `amd3dvcacheSvc.exe`/`amd3dvcacheUser.exe` (AMD optimizer),
`ProcessGovernor.exe` (Process Lasso), Windows Game Mode. When AMD owns the game layer we
stay off the game entirely; when Process Lasso is present we bound/defer our herd actions to
avoid the two-optimizers-flapping failure (F16). **No Forced-Mode-equivalent on by default.**

### 5.3 Staged milestones (E8/A4 — each a measurable on-box exit)
- **M0 — ETW repair.** Fix F-1…F-4. *Exit:* `logman query providers
  Microsoft-Windows-Kernel-Process` shows the correct GUID; launching notepad delivers a
  ProcessStart to the consumer, `events_processed() > 0`, per-provider `EnableTraceEx2`
  return code checked; birth→callback latency logged.
- **M1 — birth→placement path.** Allowlist + denylist + journal for ONE class (herd → CCD1
  CPU-Set). *Exit:* launch 50 background procs → all get the CCD1 placement logged with a
  reason (M4a); zero denylist violations; `taskkill /f` the agent → restart → journal reverts
  100 % (M1b/K2 verified by pre/post mask compare).
- **M2 — cost close.** Workload 5 (idle 300-proc flood), 10 min. *Exit:* SelfMonitor CPU
  < 0.5 %, RSS < 20 MB (M3a/M3b); measured birth→placement p95 ≤ 500 ms (M3c); reconcile
  janitor re-populates after a forced ETW gap.
- **M3 — measurement refinement + A/B.** `AbRefiner` for one cache-winner (shader-compile,
  workload 1) + one clock-winner (ordinary compile, workload 3). *Exit:* auto-generated A/B
  report (M4b); routing decision agrees with the A/B winner; M2b double-digit reproduced on
  shader-compile; M2c no > 2 % regression.
- **M4 — safety soak.** 4 h live desktop (game+OBS+Discord+browser+300 bg). *Exit:* zero
  allowlist violations, zero attributable crashes (M1a), zero audio-glitch-counter rise
  (M1c), zero SET-handle opens on AC trees (M1d).

---

## 6. Single honest weakest point

**The birth-time bet is a *prediction*, and this angle demotes the very instrument that would
correct a wrong prediction.** For a never-before-measured exe, the birth decision is a
name-keyed prior (rule default for its class). But the cache dossier is blunt: cache benefit
is **workload-intrinsic, not app-category-intrinsic** — ordinary compilation loses ~5 % on
V-Cache while shader compilation *gains* ~18 %, same category, opposite verdict [V2]; and even
the in-kernel cache-aware scheduler produces "workload-dependent, frequently null" results
[V1]. So a name-based prior can be *systematically wrong for a whole class*, and because this
architecture makes measurement deferred-and-rare, that wrong prior can sit uncorrected longer
than in a measurement-first design. This bites exactly where it hurts most: **M2a agreement,
under the M2 > M3 priority** — I have optimized for M3 (cost/latency), but M2 outranks it on
conflict. *Mitigation:* measurement is deferred but not *absent* — every allowlisted exe that
lives long enough gets one A/B on first real encounter, which permanently populates the cache,
so the wrong prior is self-correcting per exe over time; and the `BadEntry` list stops re-touch
of anything that regressed. *Residual I cannot design away:* the **first** encounter of a new
exe (and the whole cold-start of a fresh machine) rides on the static prior's quality, and if
that prior is miscalibrated for a popular class, M2a could sit near the ≥50 % floor until the
measurement backlog drains. An A1 reviewer should weigh this against a measurement-first
candidate that pays more cost up front to buy higher day-one agreement.

<!-- Made with my soul - Swately <3 -->
