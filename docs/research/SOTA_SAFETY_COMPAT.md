<!-- Phynned safety/compatibility SOTA — the blast-radius catalog for the MASS arc. -->
<!-- Everything that breaks when an external elevated agent mass-changes affinity + -->
<!-- priority over every controllable process, and the mitigation for each. -->

# SOTA — Safety & Compatibility Catalog (the MASS-arc blast-radius dossier)

**Scope.** Today Phynned touches *only* processes it classifies as `Game` (see
`observer/src/ProcessClassifier.cpp:classify()`), and it applies exactly three Win32
calls (`SetProcessAffinityMask`, `SetPriorityClass`, `SetThreadAffinityMask` via the
`phyriad::hw` FR-3/FR-9 wrappers in `action/src/ActionExecutor.cpp`). The **next arc
("MASS")** inverts that premise: automatic affinity/priority over *every controllable
process*. That inversion is where the operator's worry lives — *"cuestiones de
seguridad para aplicaciones que se verían afectadas — evitar crashes."* This document
is the catalog of what breaks and how to not break it.

**Target.** Windows 11, AMD 7950X3D (dual-CCD V-Cache).

## Verification legend (CONDUCT)

- **[V1]** primary source, fetched and read first-hand.
- **[V2]** primary source, read only as a search excerpt / abstract — gap declared inline.
- **[V3]** secondary source (third-party blog, forum folklore, aggregator) — link given.
  Known-pitfall folklore is admissible ONLY at this tier, with the link.

Every load-bearing claim is tagged. "Could not verify" is used where it is the honest answer.

---

## 0. The one structural finding that reframes the whole arc

**F0 — Process Lasso, the closest prior art, explicitly refuses to do the thing the MASS
arc proposes.** Bitsum's own FAQ: *"We do not recommend limiting the CPU affinities of
ALL processes with a broad all-inclusive rule… system processes should generally have
unrestricted choice of CPUs."* [V1]. Their default engine is **subtractive and
conservative** (ProBalance only nudges *priority*, never affinity, and only under
measured contention — F14). The MASS arc is therefore **beyond the design envelope of
the most mature tool in this space**; the safe version of "MASS" is an *allowlist of
what may be touched*, not a denylist of exceptions to touching everything. This is the
single highest-leverage design decision in the arc.

- Source: Process Lasso FAQ, bitsum.com [V1].

---

## 1. Facet 1 — Processes that must NEVER be touched (the denylist)

Phynned already has a denylist: `kSystemNames` in
`observer/include/phynned/observer/ProcessClassifier.hpp` — currently
`System, Registry, smss, csrss, wininit, winlogon, services, lsass, svchost,
fontdrvhost, dwm, explorer, SearchIndexer, WmiPrvSE, spoolsv, MsMpEng, SecurityHealth`
plus PID 0 and PID 4. **That list is correct for the game-only tool but has hard gaps
for MASS.** The gaps below are ranked by blast radius.

### F1 — Session-critical kernel-adjacent set (whole-session freeze / bugcheck)
`csrss.exe`, `wininit.exe`, `winlogon.exe`, `services.exe`, `smss.exe`, `lsass.exe`,
`System`(PID 4). Starving or mis-pinning any of these can hang the whole logon session
or force a bugcheck; `csrss` and `wininit` are marked *critical* by the kernel (their
death is a bugcheck). **Already denylisted — keep it, and never let a heuristic or
override path re-admit them.** The current `is_system_process()` guard (returns
`TargetKind::System` at classify step 1) is the right shape. Blast radius: **CATASTROPHIC**.
[V1] Win32 *Process Security and Access Rights* + the existing guard code.

### F2 — audiodg.exe (Windows Audio Device Graph Isolation) — NOT in the current denylist
This is the single most important MASS-arc gap. Restricting `audiodg.exe` affinity is a
folk remedy that helps *some* rigs and **causes crackle/dropouts/xruns on others**; and
Windows treats it as a protected audio-stack component and **resets the affinity on
service restart / resume-from-sleep** — so any pin the agent applies is both risky and
non-durable [V3]. audiodg runs the real-time audio mix graph; its threads are
MMCSS "Pro Audio"/"Audio" class (F10). An external agent that de-prioritizes it or pins
it into a busy CCD produces instantly-audible glitching. **Mitigation: add `audiodg.exe`
to the never-touch list; if audio placement is ever a feature, it must be opt-in and
measured, never part of a blanket sweep.** Blast radius: **HIGH** (instantly perceived).
[V3] ittrip.xyz audiodg guide; verdantran gist; tenforums real-time-audio thread.

### F3 — DWM / composition (dwm.exe) — already denylisted, keep it
Desktop Window Manager composites every visible window. De-prioritizing or starving it
freezes/janks the *entire* desktop, including the game's own present path on the
compositor. Already in `kSystemNames`. Blast radius: **HIGH**. [V3] Process Lasso
recommends excluding `dwm.exe`, `csrss.exe`, `fontdrvhost.exe` from ProBalance.

### F4 — Input / shell text stack: ctfmon.exe, conhost.exe — NOT in the denylist
`ctfmon.exe` runs Text Services Framework (keyboard layouts, IME, some hotkeys);
`conhost.exe` hosts every console window. De-prioritizing these produces laggy/dropped
input and console stalls that read as "the app froze." Process Lasso's community
guidance explicitly lists `conhost.exe` (and `wt.com`) as ProBalance-exclusion
candidates [V3]. **Mitigation: add `ctfmon.exe`, `conhost.exe` to never-touch.**
Blast radius: **MEDIUM-HIGH**.

### F5 — User-mode driver hosts: WUDFHost.exe (+ dllhost.exe, taskhostw.exe, sihost.exe) — NOT denylisted
`WUDFHost.exe` hosts user-mode device drivers (some audio, biometric, sensor, and
peripheral drivers). Pinning/starving it can stall device I/O. `dllhost.exe` (COM
surrogate), `sihost.exe` (shell infra), `taskhostw.exe` (scheduled-task host) are
generic hosts whose workload identity is unknown from the exe name alone — a blanket
policy cannot know what a given `dllhost.exe` is doing. **Mitigation: treat all generic
Windows *host* processes as never-touch by default; their contents are opaque.**
Blast radius: **MEDIUM**. [V3] (host-process opacity is established Windows behavior;
could not find a documented affinity-specific incident — flagged as reasoned, not proven).

### F6 — Virtualization workers: vmmem / vmmemWSL / vmwp.exe / vmcompute.exe — NOT denylisted
On the 7950X3D these are common (WSL2, Hyper-V, WSA, Docker Desktop, Android emulators).
`vmmem`/`vmmemWSL` are the *pseudo-processes* representing a VM's memory+vCPU; `vmwp.exe`
is the Hyper-V worker; `vmcompute.exe` the host compute service. Pinning a VM worker into
a 2-core mask throttles the *entire guest* (and every container inside it). **Mitigation:
add the vm* family to never-touch.** Blast radius: **MEDIUM-HIGH for VM users**.
Could not find a documented affinity-crash incident specific to `vmmem` — reasoned from
its role [V3].

### F7 — AV / EDR real-time components: MsMpEng.exe (denylisted) + third-party EDR (NOT denylisted)
Two distinct failure modes. (a) **Performance**: Process Lasso FAQ warns that
*"adjustments to real-time scanning components can have undesired effects on performance
since they block calls from other applications"* [V1] — throttling a real-time scanner
stalls every file/exec call system-wide. (b) **Detection**: see F19 — opening handles
to AV processes is a textbook credential-theft/tamper signal. `MsMpEng.exe` and
`SecurityHealth.exe` are denylisted, but **third-party EDR is not** — CrowdStrike
(`CSFalconService.exe`), SentinelOne (`SentinelAgent.exe`), Sophos, ESET, etc. These are
exactly the agents most likely to *react* to a mass-handle-opening elevated process.
**Mitigation: never touch, never even `OpenProcess`, any process whose image path is
under a security-vendor directory or matching a known-EDR name table.** Blast radius:
**HIGH** (both perf and detection).

**Facet-1 summary — additions the MASS denylist needs beyond `kSystemNames`:**
`audiodg.exe, ctfmon.exe, conhost.exe, WUDFHost.exe, dllhost.exe, taskhostw.exe,
sihost.exe, RuntimeBroker.exe, ShellExperienceHost.exe, StartMenuExperienceHost.exe,
vmmem, vmmemWSL, vmwp.exe, vmcompute.exe, MemCompression`, plus the EDR name table and
the anti-cheat service table (F9). The safe framing (F0) is an **allowlist**, so this
denylist is the second line of defence, not the first.

---

## 2. Facet 2 — Anti-cheat reactions

### F8 — The kernel mechanism: OpenProcess handles are silently stripped, not errored
EAC/BattlEye/Vanguard register a kernel **ObRegisterCallbacks** pre-operation callback
on `OB_OPERATION_HANDLE_CREATE` for the protected process. When any external process
calls `OpenProcess`, the callback inspects `DesiredAccess` and **removes the dangerous
bits before the handle is returned** — the caller gets a *reduced-rights handle*, not a
failure. Documented whitelist in a worked example keeps only
`PROCESS_VM_READ, PROCESS_QUERY_INFORMATION, PROCESS_QUERY_LIMITED_INFORMATION,
READ_CONTROL, SYNCHRONIZE`; **`PROCESS_SET_INFORMATION` and `PROCESS_VM_WRITE` are
stripped** [V2 mechanism, read first-hand on XPN's blog; V3 that EAC/BE/Vanguard
specifically do this — inferred from the EAC handle-callback description]. Consequence
for Phynned: `SetProcessAffinityMask`/`SetPriorityClass` on a protected game will fail
with **access-denied** (the handle lacks `PROCESS_SET_INFORMATION`) — this matches the
project's existing observation in `kGameNames` comments ("Vanguard blocks PROCESS_VM_READ").

### F9 — Kick / crash / refuse-to-launch is the real risk, not usually a ban
Calibrated honestly: Process Lasso — which does *external* affinity/priority exactly like
Phynned — states *"using Process Lasso will not cause bans. There has never been a single
case"* and that the worst case is *"if the rules you set are somehow problematic for a
game, it will refuse to launch"* [V1]. Anti-cheat literature distinguishes **kick
(temporary) from ban (permanent)**; non-cheat optimization/overlay tools are generally
*kicked or blocked*, not banned, unless they inject or load a vulnerable driver [V3].
Phynned loads no driver and injects nothing, so the honest risk statement is:
**access-denied / kick / launch-failure / crash, not a ban** — but "not usually" is not
"never," and kernel anti-cheats change policy without notice. The FAQ's existing
"read the AntiCheat notice, exclude competitive games" stance is correct and must
survive into MASS. Blast radius: **HIGH** (user-facing, reputational).

### F10 — THE distinction: touching the game vs. touching everything AROUND the game (the launcher-inheritance trap)
This is the subtle, documented, decisive one. **Windows process affinity is inherited by
child processes** [V1, SetProcessAffinityMask remarks]. A real Elden Ring incident: a
user restricted **Steam** (`steam.exe`) to cores 0–15; the game launched *through* Steam
inherited that mask; **EAC then failed its code-integrity check** (*"Code integrity
determined that the image hash of a file is not valid"* for the EAC DLL) and the game
**white-screen-crashed on startup**. Fix was to give Steam all cores again and exclude it
from ProBalance [V3]. Lesson for MASS: **an affinity change to a launcher/parent
propagates into the protected child and can break anti-cheat *even though the agent never
touched the game*.** Phynned already classifies launchers as `Productivity`
(`kLauncherHelperNames`) meaning "observed, never auto-pinned" — that policy is exactly
right and MASS must not weaken it. Blast radius: **HIGH** (crash on launch, non-obvious cause).

### F11 — Vanguard is the strict case (always-on kernel, boot-start)
Riot Vanguard runs a boot-start kernel driver that *"watches for things that might tamper
with the signed League process and prevents them"* and validates the environment before
granting an anti-cheat session [V3, Riot's own post + League wiki; academic treatment in
arXiv:2408.00500 "If It Looks Like a Rootkit"]. Affinity/priority manipulation of the
protected process is precisely the class it blocks. **Mitigation: Vanguard-protected
titles (Valorant, LoL) belong on a hard exclusion; do not even enumerate their modules.**
Blast radius: **HIGH**.

---

## 3. Facet 3 — Crash / starvation classes (apps that break when the core set shrinks)

### F12 — The N-threads-into-2-cores class: runtimes that size parallelism off processor count
The core hazard of MASS. Apps decide their thread/heap counts from the processor count at
startup, then the agent pins them into fewer cores afterward — creating oversubscription
that spin-heavy code turns into priority inversion, livelock, and watchdog timeouts.

- **.NET**: Pre-.NET 6 on Windows, `Environment.ProcessorCount` **ignores the affinity
  mask** and returns the machine's logical-CPU count [V2, MS Learn breaking-change doc via
  search excerpt] — so a pre-6 app spawns full-width thread pools / server-GC heaps and
  then gets crushed into a 2-core mask. .NET 6+ made `ProcessorCount` *respect* affinity,
  which fixes *newly started* processes but **not a process already running when the agent
  changes its mask** — its pools were already sized. GC server heaps
  (`GCHeapCount`/`GCHeapAffinitizeMask`) are affinitized to specific CPUs; changing the
  process mask underneath them fights that. [V2]
- **Java/JVM**: `Runtime.availableProcessors()` is read at pool-creation time;
  `ForkJoinPool.commonPool()`, `ThreadPoolExecutor`, GC and JIT thread counts derive from
  it. Changing affinity afterward does **not** resize existing pools; the JVM's own escape
  hatch is the `-XX:ActiveProcessorCount` flag [V2/V3]. An oversized pool pinned to 2
  cores = context-switch storm.
- **Native**: apps that call `GetSystemInfo().dwNumberOfProcessors` (or read
  `dwActiveProcessorMask`) once and shard work N-ways have the same failure.
- **General result**: research on thread oversubscription notes *"most applications
  crash when CPU count decreased"* and that pinning "needs to re-pin threads when CPU
  count changes" [V2, HPDC21 oversubscription paper via excerpt]. Spinlocks are the
  amplifier: a preempted lock-holder pinned onto a contended core makes every spinner
  burn its whole timeslice — classic priority inversion / starvation [V3].

**Mitigation**: (a) prefer **priority** and **CPU-Sets** (`SetProcessDefaultCpuSets`) over
hard **affinity masks** — CPU-Sets are a *soft hint* the scheduler can override, so they
cannot starve; Process Lasso itself recommends CPU-Sets over affinity for exactly this
"allow growth beyond the preferred set" reason [V3]. (b) Never shrink the mask of a
process that has already spawned wide (detect thread_count ≫ target cores).
(c) Keep the current `AutoRevertGuard` variance-regression trip (30 s window, 1.20×
threshold) as the backstop, and consider a *hang* detector (no progress) in addition to a
*variance* detector. Blast radius: **HIGH**.

### F13 — Apps that read/set their OWN affinity (the fight)
- **SQL Server** manages its own scheduler affinity. Changing its mask externally forces
  it to bring CPU schedulers online/offline dynamically; a permanent system task can be
  stranded on an *offline* scheduler, and *"a database server restart is required"* to
  fully reconfigure [V2, MS Learn affinity-mask doc via excerpt]. Not a crash, but a
  correctness/perf degradation the agent caused and cannot see.
- **Games / engines** that set their own affinity, and **licensing/DRM** software
  sensitive to topology (some node-locked licenses hash CPU/core topology) will *fight*
  or *mis-validate* when the observed core set changes.
- **The Phynned-specific angle**: `ActionExecutor::revert()` deliberately restores to
  `default_affinity_mask_` (ALL cores), *not* the captured `prev_affinity_mask`
  (`action/src/ActionExecutor.cpp:309-327`). That is correct for crash-residue hygiene on
  *games*, but for MASS it is a **bug against any app that legitimately had a narrower
  self-chosen affinity before Phynned touched it** — revert would widen it, destroying the
  app's intent. **Mitigation for MASS: never touch a process that already has a
  non-default affinity mask at observe time (it is self-managing); and if a self-managed
  process must be touched, revert must restore the captured prior mask, not the
  system default.** Blast radius: **MEDIUM** (silent misconfiguration).

### F14 — Real-time audio under QoS/EcoQoS
WASAPI **exclusive-mode**, sub-10 ms streams get the MMCSS **"Pro Audio"** task class
(≥10 ms → "Audio") to raise thread priority [V2, MS Learn exclusive-mode/low-latency
docs via excerpt]. If MASS applies **EcoQoS** (via `SetProcessInformation` +
`PROCESS_POWER_THROTTLING_EXECUTION_SPEED`) to "background" audio/DAW/voice processes to
save power, it caps CPU frequency and steers to efficiency cores — Microsoft itself notes
EcoQoS *"should not be used for performance-critical or foreground user experiences"* and
*"may cause instability for certain processes"* [V2, MS "Introducing EcoQoS" devblog via
excerpt]. Windows-11 Task-Manager Efficiency Mode = "base priority Low + EcoQoS" [V2].
**Mitigation: EcoQoS is the safest *background* lever (it cannot fully starve, the
scheduler still runs the thread), BUT it must never be applied to any audio-graph, DAW,
communication, or foreground process.** On the 7950X3D there are no true E-cores, so
EcoQoS mostly caps frequency rather than relocating — lower blast radius here than on
Intel hybrid, but the instability caveat stands. Blast radius: **MEDIUM**.

---

## 4. Facet 4 — Oscillation / feedback (the control loop)

### F15 — Consumption-reactive placement is a feedback loop that will thrash
If placement decisions are driven by measured consumption, moving a process changes its
consumption, which changes the next decision → oscillation. Prior art:

- **ProBalance is the reference restraint design** [V1, bitsum "how ProBalance works"]:
  it acts **only under high CPU load**, **only lowers priority class** (to Below Normal,
  "a marginal change"), the change is **temporary — undone as soon as conditions change**,
  it **skips the foreground process by default**, and it **ignores processes that set
  their own priority class**. It publishes *no* exact thresholds/timings deliberately.
  The takeaways for Phynned: (i) act on a *dwell/hysteresis* window, not per-tick;
  (ii) prefer the reversible, non-starving lever (priority) over the hard one (affinity);
  (iii) exempt foreground and self-managed processes.
- **Control-theory basics**: this is a bang-bang controller and needs **hysteresis** (two
  thresholds — a high to engage, a lower to disengage) plus a **cooldown/dwell timer** so
  a process cannot be re-placed until it has been stable for T seconds. Phynned's current
  `AdaptiveTick` (100 ms tick) is far too fast to be the placement cadence — placement
  decisions should run on a slower, hysteretic sub-clock. Blast radius: **MEDIUM**
  (perf thrash, not crash). [V3 control-theory framing.]

### F16 — Multiple optimizers fighting (Phynned + Process Lasso + Windows + AMD driver)
Real and common on X3D rigs. Windows **Game Mode / Game Bar** and AMD's V-Cache
**"3D V-Cache Optimizer"** driver both steer game threads to the V-Cache CCD; users
report needing to disable Game Bar so Process Lasso rules take "sole control," and
Process Lasso offers a **"Forced Mode"** that *continuously re-applies* rules precisely
because other actors keep changing affinity back [V1 bitsum FAQ + V3 overclockers/steam
threads]. Two continuous re-appliers = a tug-of-war that pins the target's affinity in a
flapping loop. **Mitigation: detect coexisting managers before acting** — check for the
AMD V-Cache optimizer service, `ProcessGovernor.exe` (Process Lasso's engine), and
Windows Game Mode being active; when detected, **defer** (Process Lasso's own default is
to *defer* to processes/tools that set a conflicting affinity unless Forced Mode is on).
Do **not** ship a Forced-Mode equivalent on by default. Blast radius: **MEDIUM**.

---

## 5. Facet 5 — Revert guarantees

### F17 — Affinity/priority PERSIST after the setter dies (verified) — the SIGKILL gap
**Confirmed [V1]**: an affinity mask is a property *of the target process*, stored in the
target and *"inherited by any child process,"* set via a handle that merely needs
`PROCESS_SET_INFORMATION` (SetProcessAffinityMask remarks). Nothing ties the mask's
lifetime to the *caller*. Therefore: **if Phynned sets a game/app's affinity and is then
`TerminateProcess`-killed (Task Manager "End task", `taskkill /f`, OOM-killer, a crash
that skips C++ destructors), the target keeps the restricted mask indefinitely** — a
leaked pin the user cannot easily see. The current design reverts via
`ActionExecutor::~ActionExecutor() → revert_all()` (RAII) and "everything reverts on exit,
crash included" (ARCHITECTURE.md) — **but a C++ destructor does NOT run on
`TerminateProcess`/SIGKILL**, so that guarantee holds only for graceful exit and C++
exceptions, not for a hard kill. This is a genuine gap that MASS (which leaves marks on
*many* processes) makes much worse. Blast radius: **HIGH for MASS** (many leaked pins).

**Mitigations (SOTA patterns):**
- **Journal-based revert on restart.** Phynned already writes `audit.bin` (append-only
  action log) and an in-memory `ActionLog` ring. Persist *enough* of each applied action
  (pid, exe-identity, prev mask, prev priority) to a small **on-disk journal** flushed on
  apply; on next agent start, **replay the journal and revert any still-live pins** before
  doing anything else. This survives SIGKILL because the recovery is at *next boot of the
  agent*, not in the dying process. (Note: raw PID is unreliable across the gap — PIDs
  recycle; key the journal on exe-identity + start-time or reconcile against the live
  process's *current* mask.)
- **Watchdog/heartbeat.** The UI already Job-Object-owns the agent (kills it on UI close).
  Invert one direction: a lightweight **watchdog that reverts-all if the agent's heartbeat
  stops** (a separate tiny process, or the service, holding the revert journal). This is
  the standard "dead-man's switch" pattern.
- **Windows Job Object as the automatic net.** If the agent assigns each touched process
  to a Job (or, cleaner, keeps its *own* affinity effects expressible as revertible state),
  Job teardown on agent death can undo *some* effects — but note affinity set via
  `SetProcessAffinityMask` is not auto-reverted by Job death, so the journal remains the
  real guarantee.
- **Revert to captured-prior, not system-default, for self-managed apps** (see F13).

### F18 — The current revert-to-all-cores choice is a deliberate tradeoff to re-examine for MASS
`ActionExecutor` reverts to `default_affinity_mask_` on purpose, to avoid propagating a
residual mask left by a *previously crashed* Phynned instance
(`action/src/ActionExecutor.cpp:33-39, 309-327`). For a game-only tool that is sound. For
MASS it collides with F13/F17: the journal-replay recovery (F17) is the better mechanism —
it distinguishes "a mask *we* set and must undo" from "a mask the *app* set and we must
preserve," which the blunt revert-to-default cannot. Blast radius: **MEDIUM**.

---

## 6. Facet 6 — The elevation surface

### F19 — Mass handle-opening by an elevated unsigned binary escalates AV/EDR heuristics
Defender and EDRs flag *"unusual handle patterns"* and *"less common interaction
patterns"* — the LSASS case is the archetype: any process opening a handle to a sensitive
process with broad rights is scored as credential-dumping/tampering, via behavior
monitoring + ML, *even if the tool is benign* [V3, Microsoft Security blog + Elastic/
Detection.FYI rules]. A MASS agent that iterates the *entire* process table calling
`OpenProcess` on hundreds of processes per tick presents exactly the "enumerate-everything
+ open-handles-everywhere" shape that behavior heuristics reward. WINDOWS_DISTRIBUTION.md
§2.2 already documents that Phynned's *current* API surface
(`SetProcessAffinityMask` + `OpenProcess(PROCESS_SET_INFORMATION)` on other users'
processes + ETW kernel session) is *"textbook signals for cheats, miners, and FPS-booster
grey-market tools."* MASS multiplies the volume. **Mitigations:**
- **Least privilege on the handle.** Open with the *minimum* access: for a pin you need
  only `PROCESS_SET_INFORMATION` (+ `PROCESS_QUERY_LIMITED_INFORMATION` to read the prior
  mask). **Never `PROCESS_ALL_ACCESS`.** `PROCESS_VM_READ`/`PROCESS_VM_WRITE` are the bits
  that most strongly trigger heuristics and anti-cheat stripping — Phynned already avoids
  VM_WRITE; keep VM_READ scoped to the D3D-module probe only, and skip that probe entirely
  for the MASS sweep.
- **Never open a handle to lsass/EDR/AV/anti-cheat at all** (F7/F11) — not even to query.
  The safest handle is the one never requested.
- **Close every handle immediately.** At MASS scale a per-tick handle leak becomes a
  self-inflicted resource exhaustion *and* a bigger detection footprint. The current
  `check_d3d_vk_modules()` correctly `CloseHandle`s in-scope; audit that every MASS path
  does likewise (RAII handle wrapper recommended).
- **Do not inherit handles.** Pass `bInheritHandles = FALSE`; a leaked inheritable handle
  to a game process is both a leak and a suspicion vector.
- Blast radius: **MEDIUM-HIGH** (false-positive quarantine of the agent; reputational).

### F20 — Unsigned + elevated + mass-touch compounds SmartScreen/Defender ML
The existing sign-ing gap (WINDOWS_DISTRIBUTION.md §2.7 — SignPath pending, no EV cert)
means near-zero reputation. Unsigned + auto-elevating + now touching *every* process is
the worst-case reputation profile. **Mitigation: the code-signing milestone moves from
"nice to have" to a *prerequisite* for shipping the MASS behavior**; and the per-binary
Defender exclusion script becomes load-bearing rather than optional. [V2/V3, project docs
+ Defender heuristic literature.]

---

## Ranked findings — by blast radius (the return set)

| # | Finding | Facet | Blast radius | In current denylist? |
|---|---|---|---|---|
| F0 | Process Lasso, the mature prior art, refuses blanket all-process affinity → MASS should be an **allowlist**, not a deny-everything | design | **reframes arc** | n/a |
| F1 | csrss/wininit/winlogon/services/smss/lsass/System = session-critical / bugcheck-on-death | 1 | CATASTROPHIC | yes ✓ |
| F17 | Affinity/priority **persist after the setter is SIGKILLed** — RAII revert doesn't cover `TerminateProcess`; MASS leaks many pins | 5 | HIGH (×N) | n/a — design gap |
| F10 | Affinity is **inherited by children**; touching a *launcher* breaks the game's anti-cheat (Elden Ring EAC integrity crash) | 2 | HIGH | launchers=Productivity ✓ |
| F2 | **audiodg.exe** — audio-stack glitch/xruns; Windows resets it anyway | 1 | HIGH (audible) | **NO — gap** |
| F7 | AV/EDR real-time components — perf stall of all apps **and** tamper-detection; 3rd-party EDR uncovered | 1/6 | HIGH | MsMpEng ✓, EDR **NO** |
| F12 | N-threads-into-2-cores: .NET/Java/native size pools off proc-count → oversubscription, spinlock priority inversion, "most apps crash when core count drops" | 3 | HIGH | n/a |
| F9 | Anti-cheat → **kick/crash/refuse-to-launch** (not usually ban); risk real, policy-volatile | 2 | HIGH | FAQ policy ✓ |
| F11 | Vanguard: always-on kernel, blocks tampering with signed process | 2 | HIGH | partial (LoL in kGameNames) |
| F19 | Mass `OpenProcess` by unsigned elevated binary trips AV/EDR handle heuristics | 6 | MED-HIGH | n/a |
| F3 | DWM — desktop-wide freeze if starved | 1 | HIGH | yes ✓ |
| F4 | ctfmon/conhost — input & console stalls | 1 | MED-HIGH | **NO — gap** |
| F6 | vmmem/vmwp — throttles entire guest/containers | 1 | MED-HIGH (VM users) | **NO — gap** |
| F8 | OpenProcess handles silently **stripped** of PROCESS_SET_INFORMATION by AC kernel callback (not errored) | 2 | (mechanism) | n/a |
| F16 | Multiple optimizers fighting (PL Forced Mode / Game Bar / AMD driver) → affinity flapping | 4 | MEDIUM | n/a |
| F15 | Reactive placement thrash; need hysteresis+cooldown; ProBalance restraint = the model | 4 | MEDIUM | n/a |
| F13 | Self-managing apps (SQL Server schedulers offline; DRM topology hashing) fight the agent | 3 | MEDIUM | n/a |
| F14 | EcoQoS/QoS on audio/DAW = instability; MS says don't use it for perf-critical/foreground | 3 | MEDIUM | n/a |
| F18 | Revert-to-all-cores (deliberate for games) is wrong for self-managed apps under MASS | 5 | MEDIUM | n/a — design |
| F5 | Generic host processes (WUDFHost/dllhost/taskhostw) are opaque; don't touch by name | 1 | MEDIUM | **NO — gap** |
| F20 | Unsigned+elevated+mass-touch = worst reputation profile; signing becomes prerequisite | 6 | MEDIUM | signing pending |

**Denylist deltas the MASS arc must add to `kSystemNames`** (F0's allowlist is still the
primary control): `audiodg.exe, ctfmon.exe, conhost.exe, RuntimeBroker.exe,
WUDFHost.exe, dllhost.exe, taskhostw.exe, sihost.exe, ShellExperienceHost.exe,
StartMenuExperienceHost.exe, vmmem, vmmemWSL, vmwp.exe, vmcompute.exe, MemCompression`
+ an EDR name/path table + an anti-cheat service table
(`EasyAntiCheat.exe, EasyAntiCheat_EOS.exe, BEService.exe, BEDaisy.sys-host, vgc.exe,
vgtray.exe`).

---

## Sources

- Microsoft Learn — [SetProcessAffinityMask](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setprocessaffinitymask) — affinity inherited by children; needs PROCESS_SET_INFORMATION; process property. **[V1, fetched]**
- Bitsum — [How ProBalance Works](https://bitsum.com/how-probalance-works/) — lowers priority (not affinity), temporary, skips foreground/self-prioritized. **[V1, fetched]**
- Bitsum — [Process Lasso FAQ](https://bitsum.com/process-lasso-faq/) — no bans/worst-case-refuse-to-launch; don't limit ALL processes; AV-scan warning; Governor persistence; EAC inheritance/registry workarounds; Forced Mode. **[V1, fetched]**
- Bitsum community — [ProBalance exclusions: when to exclude](https://community.bitsum.com/forum/index.php?topic=4880.0) / [ProBalance system-process guidance](https://bitsum.com/how-probalance-works/) — audiodg/csrss/dwm/fontdrvhost/lsass; conhost/wt.com. **[V3]**
- XPN InfoSec — [Windows Anti-Debug: OpenProcess filtering](https://blog.xpnsec.com/anti-debug-openprocess/) — ObRegisterCallbacks strips PROCESS_SET_INFORMATION / PROCESS_VM_WRITE from DesiredAccess; caller gets reduced-rights handle, not an error. **[V2 mechanism, fetched; V3 for EAC/BE/Vanguard specificity]**
- Steam Community (Elden Ring) — [White-screen crash + Process Lasso](https://steamcommunity.com/app/1245620/discussions/0/6725643618953557568/) — restricted Steam affinity inherited by game → EAC image-hash/code-integrity failure → crash; fix = give launcher all cores. **[V3, forum incident]**
- Riot Games — [Vanguard On-Demand](https://www.riotgames.com/en/news/vanguard-on-demand) + [League Wiki: Riot Vanguard](https://wiki.leagueoflegends.com/en-us/Riot_Vanguard) — kernel driver watches/prevents tampering with the signed game process. **[V3]**
- arXiv:2408.00500 — [If It Looks Like a Rootkit… Kernel-Level Anti-Cheat](https://arxiv.org/pdf/2408.00500) — academic treatment of EAC/BE/Vanguard kernel behavior. **[V3]**
- MPGH / GuidedHacking — [handle-hijacking multi-AC bypass](https://www.mpgh.net/forum/showthread.php?t=1343255) / [detecting hidden processes](https://guidedhacking.com/threads/how-to-detect-hidden-processes-anticheat-feature.20644/) — EAC/BE handle-callback behavior, "uncommon handles = sign of abuse." **[V3]**
- Microsoft Learn — [Environment.ProcessorCount .NET 6 breaking change](https://learn.microsoft.com/en-us/dotnet/core/compatibility/core-libraries/6.0/environment-processorcount-on-windows) + [issue #47427](https://github.com/dotnet/runtime/issues/47427) — pre-6 Windows ignores affinity; 6+ respects it. **[V2, search excerpt]**
- Microsoft Learn — [GCHeapAffinitizeMask](https://learn.microsoft.com/en-us/Dotnet/framework/configure-apps/file-schema/runtime/gcheapaffinitizemask-element) / [GCHeapCount](https://learn.microsoft.com/en-us/dotnet/framework/configure-apps/file-schema/runtime/gcheapcount-element) — server-GC heap affinitization. **[V2, search excerpt]**
- JVM — [-XX:ActiveProcessorCount](https://answers.ycrash.io/question/what-is-jvm-runtime-tuning--xxactiveprocessorcount) + [CPU considerations for Java in containers](https://christopher-batey.medium.com/cpu-considerations-for-java-applications-running-in-docker-and-kubernetes-7925865235b7) — availableProcessors drives ForkJoinPool/GC/JIT sizing. **[V3 / V2 for the flag]**
- Microsoft Learn — [affinity mask (SQL Server)](https://learn.microsoft.com/en-us/sql/database-engine/configure-windows/affinity-mask-server-configuration-option) — external affinity change → schedulers offline, permanent task stranded, restart may be required. **[V2, search excerpt]**
- Microsoft Learn — [Exclusive-Mode Streams](https://learn.microsoft.com/en-us/windows/win32/coreaudio/exclusive-mode-streams) / [Low Latency Audio](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio) — WASAPI exclusive < 10 ms → MMCSS "Pro Audio". **[V2, search excerpt]**
- Microsoft devblogs — [Introducing EcoQoS](https://devblogs.microsoft.com/performance-diagnostics/introducing-ecoqos/) + [SetProcessInformation](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation) — PROCESS_POWER_THROTTLING_EXECUTION_SPEED; "not for perf-critical/foreground," "may cause instability." **[V2, search excerpt]**
- ittrip.xyz — [Permanently set CPU affinity/priority for audiodg.exe](https://en.ittrip.xyz/windows11/audiodg-cpu-affinity) + [verdantran gist](https://gist.github.com/verdantran/a1dea9d31f5dbf60224dff500f2658c2) + [tenforums real-time audio](https://www.tenforums.com/sound-audio/178473-solution-audio-glitches-how-ensure-general-real-time-performance.html) — audiodg single-core folk remedy; Windows resets affinity on restart/resume. **[V3]**
- Microsoft Security blog — [Detecting/preventing LSASS credential dumping](https://www.microsoft.com/en-us/security/blog/2022/10/05/detecting-and-preventing-lsass-credential-dumping-attacks/) + [Elastic: Suspicious LSASS access](https://www.elastic.co/guide/en/security/8.19/suspicious-lsass-process-access.html) + [Detection.FYI OpenProcess-on-lsass rule](https://detection.fyi/elastic/detection-rules/windows/credential_access_lsass_openprocess_api/) — behavior/ML heuristics on unusual OpenProcess handle patterns. **[V3 / V2 for the MS blog]**
- Overclockers UK — [7950X3D best setup](https://forums.overclockers.co.uk/threads/7950x3d-best-setup.18977318/) + Process Lasso FAQ — disable Game Bar for PL sole control; Forced Mode re-applies rules. **[V3 + V1]**
- HPDC '21 — [Exploiting CPU Elasticity via Efficient Thread Oversubscription](https://ranger.uta.edu/~jrao/papers/HPDC21-1.pdf) — "most applications crash when CPU count decreased"; pinning must re-pin on core-count change. **[V2, excerpt]**
- siliceum — [Spinning around: Please don't!](https://www.siliceum.com/en/blog/post/spinning-around/) — spinlock priority inversion / starvation when preempted or oversubscribed. **[V3]**

<!-- Made with my soul - Swately <3 -->
