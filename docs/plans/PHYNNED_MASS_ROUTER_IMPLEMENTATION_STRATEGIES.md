<!-- PLAN_TIER T1/T2 artifact — the HOW. Per-strategy concrete edit sites (real verified paths + -->
<!-- approx line anchors), each mitigating edit site tagged with the risk ID it closes. -->
<!-- FDP applies: line anchors are marked "approx" where they will drift; every file path is real -->
<!-- and was read first-hand while drafting this. Sibling: PHYNNED_MASS_ROUTER_MASTER_PLAN.md. -->

# Phynned Mass-Router — Implementation Strategies

**Tier: 2.** Sibling plan: [`PHYNNED_MASS_ROUTER_MASTER_PLAN.md`](PHYNNED_MASS_ROUTER_MASTER_PLAN.md).
Risk register (risk IDs referenced below): [`PHYNNED_MASS_ROUTER_RISK_REGISTER.md`](PHYNNED_MASS_ROUTER_RISK_REGISTER.md).
Architecture spine: [`PHYNNED_MASS_ROUTER_ARCHITECTURE.md`](PHYNNED_MASS_ROUTER_ARCHITECTURE.md).

**Risk IDs used at edit sites** (assigned by the supervisor in the register): **CR1**
AC-descendant SET-open · **CR2** ETW session crash/zombie · **DR1** journal-revert survives
`taskkill` · **OR1** oscillation / optimizer-fight · **SR1** mass-handle-open AV/EDR detection ·
**PR1** self-managed-mask widening · **BR1** uint8 slot / `kMaxTargets` caps.

Line anchors were read first-hand on 2026-07-17 and are marked **approx** — they will drift as
edits land; the surrounding symbol names are the durable anchor. Verify the symbol, not the number.

Status: **DRAFT.** No code written. Strategy order = milestone order (S0 first; safety before breadth).

---

## S0 — ETW repair (M0 prerequisite; enables the birth stream and CR2 hardening)

The corrected Kernel-Process birth stream is prerequisite to everything; the architecture names
four repairs (§4). All four are in the vendored ETW pillar + its one caller.

**Edit sites:**
- `framework/etw/include/phyriad/etw/SessionManager.hpp` **~L159–161** — `providers::kKernelProcess`
  is `0x0268a8b6-74fd-4302-9b4a-6ea0fbb19d9e`, which is **not** the Microsoft-Windows-Kernel-Process
  provider GUID. Replace with the correct GUID. **Do not hardcode from memory** — confirm on the
  box first: `logman query providers "Microsoft-Windows-Kernel-Process"` (the canonical value is
  believed to be `{22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}` — verify before committing).
- `framework/etw/include/phyriad/etw/SessionManager.hpp` **~L170–172** — `providers::kKernelContextSwitch`
  is `0xdef2fe46-7bd6-4b80-bd94-f57fe20d0ce3`, which is the **StackWalk** MOF class GUID, *not*
  context-switch. Replace with the correct context-switch source for the system-logger contract
  (verify the GUID/keyword on-box). **mitigates CR2** (a wrong GUID silently yields an empty
  stream that the janitor then has to paper over — the lossy-tree failure mode).
- `framework/etw/src/SessionManager.cpp` **~L47** (`init_props`) — `p->LogFileMode =
  EVENT_TRACE_REAL_TIME_MODE;` is missing `EVENT_TRACE_SYSTEM_LOGGER_MODE`. Add it so
  kernel/system providers (CSwitch) can be enabled on this private session. **mitigates CR2.**
- `framework/etw/src/SessionManager.cpp` **~L98–111** — the `EnableTraceEx2(...)` return value is
  discarded (the loop comment says "non-fatal — log but continue" but nothing is even captured).
  Capture `const ULONG er = EnableTraceEx2(...)`, log `er` per provider, and surface a
  per-provider enable-status so the caller can tell "session up but Kernel-Process failed to
  enable" from "all good." **mitigates CR2** (a silently-failed enable currently looks identical
  to success).
- `observer/src/MetricsCollector_win32.cpp` **~L96–114** — the `ProviderSpec[]` array feeds the
  broken GUIDs and keyword `0x10` into `etw_session_.start("PhynnedKernel.v1", …)`. After the GUID
  fix, re-derive the CSwitch keyword for the corrected source and confirm `events_processed() > 0`
  on the box. This is the M0(a) exit measurement site.

**M0(a) proof:** a short on-box run logging `SessionManager::events_processed()` and the birth
event count — non-zero birth stream on the corrected GUIDs.

---

## S1 — Admission gauntlet + zero-handle AC detection + least-privilege cached probe (M0 safety veto; CR1 / SR1 / PR1)

The default-deny gate. Nothing reaches a `Set*`/CPU-Set call unless positively admitted, and
nothing anti-cheat-adjacent is ever handle-opened. This is the M1-veto machinery and the largest
new surface. Reflects the operator's amended K1/M1d (2026-07-17): a **single least-privilege
probe** (try-then-back-off) replaces conservative abstention on non-(c) titles.

**New modules:**
- `observer/ProcessTree` (`.hpp`/`.cpp`) — incremental tree keyed on birth-time `ParentPID`.
  The raw data already exists: `observer::TargetProcess::parent_pid`
  (`observer/include/phynned/observer/TargetProcess.hpp` **~L37**) is populated by
  `ProcessObserver::update_target(pid, parent_pid, name)`
  (`observer/src/ProcessObserver.cpp` **~L113, ~L137, ~L179**). The tree adds ancestry + descendant
  walks over that field at zero query cost. **mitigates CR1** (the descendant-refusal gate needs
  a real tree; the field alone is not a tree — there is currently **no** ancestry/descendant walk
  anywhere in the codebase, verified by grep).
- `observer/AcDriverOracle` (`.hpp`/`.cpp`) — **the zero-handle detection + class oracle.**
  Enumerate loaded kernel drivers + running services system-wide (`Win32_SystemDriver` /
  `EnumDeviceDrivers` + SCM — confirmed on-box 2026-07-17: 182 drivers / 116 services readable with
  **zero handle on any game process**) and match a static **driver-to-class map**: `vgk.sys`=
  Vanguard=(b); EAC driver=(a); Ricochet / kernel-BattlEye driver=(c). Decision: no AC or a known
  (a)/(b) driver = probe-allowed; a known (c) driver OR **any unrecognized AC driver active on the
  foreground game** = do-not-probe (fail-safe). Seeds from the [V]-tagged
  `SOTA_ANTICHEAT_AFFINITY.md`; everything unmapped falls to the conservative default. **mitigates
  CR1.**
- `observer/AdmissionGauntlet` (`.hpp`/`.cpp`) — the ordered chain
  `denylist → PPL → AcDriverOracle do-not-probe check → self-managed-mask exemption → launcher/host →
  audio/foreground → positive class match → coexistence-defer`. Returns admit/refuse + reason
  (the reason feeds M4a). **mitigates CR1, PR1.**

**Edit sites:**
- `observer/include/phynned/observer/ProcessClassifier.hpp` **~L180** (`kSystemNames`) — extend the
  denylist with the AC **service/driver** names (the current list has `MsMpEng.exe` but no
  EAC/BE/Vanguard entries). Keep `kLauncherHelperNames` (**~L133**) and `kGameNames` (**~L121**) as
  the allowlist/denylist base. **mitigates CR1.** Note the self-managed-mask exemption and PPL
  refusal are new gauntlet stages, not classifier lists.
- `core/src/AgentRuntime.cpp` **~L811–838** (auto-discovery probe) — today `check_d3d_vk_modules(fg_pid)`
  opens the process with `PROCESS_VM_READ` (the strongest right Phynned uses) **before** the
  `is_os_shell_process` / denylist check. Reorder so denylist + `AcDriverOracle` run **before any
  `OpenProcess`**, and **replace the `PROCESS_VM_READ` module-sniff** on any AC-adjacent title (it
  is exactly the cheat-shaped handle the AC watches — [V] SOTA_ANTICHEAT F8). **mitigates CR1, SR1.**
- **NEW — the least-privilege probe** in `action/src/ActionExecutor.cpp` at the `OpenProcess` site:
  for a probe-allowed, admitted PID, open with **`PROCESS_SET_INFORMATION` (0x0200) ONLY** (never
  `VM_READ`/`ALL_ACCESS`); attempt the placement once; on `ACCESS_DENIED` write **BLOCKED** to
  `learn/EvidenceStore` (PerGameMemory) and never re-probe; on success write **ALLOWED**. Keyed on
  exe-identity, one probe per title ever. **mitigates CR1, SR1.** (Default-off byte-identical: with
  the mass path disabled no probe handle is opened.)
- `core/src/AgentRuntime.cpp` policy-apply loop **~L1084–1150** — insert the gauntlet as the gate
  in front of `executor.apply(...)` / `apply_differential_pin(...)`; a refused PID never reaches a
  setter, and a BLOCKED-labeled exe never re-probes. **mitigates CR1, PR1.**

**M0(b) proof (three-part on-box, operator-provided games):** (1) **(a) EAC (Halo MCC):** probe gets
`ACCESS_DENIED`, is caught, exe labeled BLOCKED, `audit.bin` shows the open happened **exactly once**
across repeated launches. (2) **(b) Vanguard (LoL):** probe succeeds, routing applies, full session
with **zero kicks**. (3) **(c)/unknown:** `audit.bin` shows **zero handle opens** on a CoD/Destiny
title, its tree, or its AC driver.

---

## S2 — Write-ahead journal + external dead-man (M1b; DR1 / CR1)

Affinity persists after the setter process dies ([V1], objective M1b), so RAII revert is
insufficient. The journal + external reverter is the crash-recovery spine. **Neither exists
today** — grep found "journal / dead-man / phynned-revert" only in the design docs, not in code.

**New modules:**
- `action/PlacementJournal` (`.hpp`/`.cpp`) — on-disk record, one entry per placement, states
  `PENDING → APPLIED → REVERTED`. Written **before** the `Set*` (PENDING) and updated **after**
  (APPLIED). Stores `pid`, captured `prev_affinity_mask`, captured `prev_cpu_set`, captured
  `prev_priority`, and applied values. **mitigates DR1.**
- `tools/phynned-revert` (new executable) — the dead-man. On launch it reads the journal and
  reverts every `APPLIED`/`PENDING` entry to its captured-prev, then marks it `REVERTED`. Invoked
  after `taskkill /f` of the agent (and registered so a service supervisor can run it on agent
  death). **mitigates DR1** (RAII in `~ActionExecutor` — `core/src/AgentRuntime.cpp` **~L346–353**,
  **~L1238** — cannot fire on `taskkill /f`; the external reverter is the only path that survives).

**Edit sites:**
- `action/src/ActionExecutor.cpp` `apply()` **~L118–172** and `apply_differential_pin()`
  **~L174–258** — write the journal PENDING entry *before* calling `phyriad::hw::set_*`, flip to
  APPLIED after success. The captured prev-mask returned by `set_process_affinity` (**~L98**) is the
  journal's revert target. **mitigates DR1.**
- `action/src/ActionExecutor.cpp` `revert()` / `revert_all()` — mark journal entries REVERTED so a
  later dead-man run does not double-revert a healthy shutdown. **mitigates DR1.**

**M1b proof:** apply placements, `taskkill /f` the agent, run `phynned-revert`, compare pre/post
masks — 100% of journaled placements reverted (objective M1b).

---

## S3 — CPU-Set / affinity / EcoQoS `phynned::hw` wrappers + revert-to-captured-prev (spine levers; BR1 / DR1 / OR1)

The soft mass lever (herd → CCD1) and the corrected revert semantics. **No CPU-Set / EcoQoS /
power-throttling code exists anywhere today** — verified by grep across all `.hpp`/`.cpp`. These
wrappers are genuinely new.

**New file (in the existing topology pillar):**
- `framework/topology/CpuSetControl` (`.hpp` + `src`) — free functions alongside FR-3/9:
  - `set_process_default_cpu_sets(pid, cpu_set_ids)` wrapping `SetProcessDefaultCpuSets` — the
    **soft** herd lever. Open the handle with **`PROCESS_SET_LIMITED_INFORMATION`**, deliberately
    *not* `PROCESS_SET_INFORMATION` (the existing FR-3 affinity wrapper uses the heavier right at
    `framework/topology/src/HardwareTopology.cpp` **~L1250–1251**, and FR-9 at **~L1349–1351**).
    The lighter right cannot fail the same way and avoids the `PROCESS_TERMINATE`-adjacent bits AV/EDR
    heuristics flag. **mitigates SR1.**
  - `set_process_eco_qos(pid, on)` wrapping `SetProcessInformation(ProcessPowerThrottling, …)` —
    optional herd de-prioritization. Reads/restores prior throttling state for revert.

**Edit sites (the revert-to-captured-prev correction — the F18/F13 MASS bug):**
- `action/src/ActionExecutor.cpp` `revert()` **~L309, ~L321–322** — currently restores
  `default_affinity_mask_` (all cores), **not** the captured `prev_affinity_mask`. `revert_all()`
  does the same at **~L376–377**. The header documents this as a *deliberate* single-target choice
  (`action/include/phynned/action/ActionExecutor.hpp` **~L138–154**: restoring captured-prev could
  propagate a crash residual). Change to **revert-to-captured-prev**, sourced from the S2 journal so
  it is crash-safe. **mitigates DR1** — and see §Finding-1: this correction is only sound *because*
  S2 exists; do not land S3's revert change ahead of the journal.
- `action/src/ActionExecutor.cpp` — add the CPU-Set + EcoQoS apply/revert paths symmetric to the
  affinity path, capturing prior CPU-Set/throttle state into the journal. **mitigates DR1.**

**Coexistence defer (OR1):**
- New `observer/CoexistenceDetector` (`.hpp`/`.cpp`) — detect the AMD 3D V-Cache Optimizer service,
  Process Lasso, and Windows Game Mode (objective E5). The gauntlet's final `coexistence-defer`
  stage consults it: when a rival optimizer is active on a PID, Phynned defers (no Forced-Mode).
  **mitigates OR1** (two-optimizers-fighting is the documented flapping class).

**Byte-identical-off discipline:** the CPU-Set and EcoQoS levers default OFF; with the mass flag
off, `set_process_default_cpu_sets` / `set_process_eco_qos` are never called and no journal entry
is written — the build's observable output is byte-identical to pre-MASS.

---

## S4 — Measurement + A/B evidence gate (M2 / M4; K4)

The list-growth mechanism: an exe is admitted to the Cache-winner allowlist only after on-box A/B
evidence passes. Cycles-per-unit-work is the always-available arbiter (ON_BOX §4 vindicated
wall-time as the *wrong* instrument).

**Edit sites:**
- `observer/include/phynned/observer/MetricsCollector.hpp` — add a `QueryProcessCycleTime`-based
  per-PID cycles sampler (near-unprivileged, objective §4.1). This is the M2a ground-truth signal;
  it is immune to the clock confound that made ON_BOX §4 inconclusive. Keep the ETW CSwitch ring
  (**~L74–96**) for migrations only.
- `bench/ABRunner` (`bench/src/ABRunner.cpp`, `bench/include/phynned/bench/ABRunner.hpp`) — extend
  to run a per-exe A/B (CCD0 vs CCD1 vs untouched) in cycles-per-work, produce a verdict, and
  **auto-generate the per-app report** (M4b) into `docs/reports/`.
- New `learn/EvidenceStore` (`.hpp`/`.cpp`), backed by the extended `learn/PerGameMemory`
  (`learn/src/PerGameMemory.cpp`) — the per-exe evidence record the gauntlet's `positive class
  match` stage consults. Admission to Cache-winner requires a passing A/B verdict here.
- `core/src/AgentRuntime.cpp` A/B wiring — `bench::ABRunner ab_runner` already exists as an Impl
  member (**~L262**) driven by IPC; route its verdicts into the EvidenceStore.

Every admitted decision logs its reason (rule hit or the A/B cycle values) via `action/AuditLog`
(**M4a**). The **burst PMC accelerator is deferred** (Master Plan §5): adopt only if it correlates
with the cycles arbiter on this box.

---

## S5 — Cap widening (MASS scale; BR1)

MASS moves from tens of curated targets to hundreds of admitted PIDs. Three cap sites plus one
**latent bug** that already bites at the current cap.

**Edit sites:**
- `observer/include/phynned/observer/TargetProcess.hpp` **~L65** — `kMaxTargets = 64u`. Widen to the
  MASS ceiling (choose against the §5 idle-flood ~300-process case; size the SHM `targets[]` /
  `metrics[]` arrays accordingly — the header's own comment sizes them off `kMaxTargets`).
  **mitigates BR1.**
- `observer/include/phynned/observer/MetricsCollector.hpp` **~L214–218** — `PidHashEntry::slot_plus_1`
  is `uint8_t`, ceiling **254** slots (slot_idx + 1 must fit in 255). Widen to `uint16_t`. And
  **~L212** `kPidHashSize = 128u` (2× the old 64) must grow to keep the ~50% load factor.
  **mitigates BR1** (past ~254 targets the slot silently truncates).
- `action/include/phynned/action/ActionExecutor.hpp` **~L126, ~L131** — `kMaxActiveActions = 32u`
  and `kMaxActiveThreadActions = 32u`. Widen for MASS; `apply()` returns `BufferFull` at the cap
  today (`action/src/ActionExecutor.cpp` **~L126–128**). **mitigates BR1.**

**Finding-2 (latent bug, real, flag it — see §Findings):** `core/src/AgentRuntime.cpp` **~L1012,
~L1027, ~L1057** use `uint32_t memory_hit_mask` with `1u << i` where `i` ranges over `n_targets`.
With `kMaxTargets = 64` **already**, `1u << i` for `i ≥ 32` is undefined behavior — this is
mis-bounded *now*, before any widening. Replace the `uint32_t` bitmask with a per-target `bool`
array (or `std::bitset<kMaxTargets>` / two `uint64_t` words) sized to the new cap. **mitigates BR1.**

**Stack-buffer sites that scale with `kMaxTargets`** (must move off-stack or be re-sized when the
cap grows to hundreds): `core/src/AgentRuntime.cpp` `live_pids[kMaxTargets]` (**~L875**),
`pids[kMaxTargets]` (**~L885**), `filtered_targets[kMaxTargets]` / `filtered_metrics[kMaxTargets]`
(**~L1052–1053**), `s_last_logged_hot_tid[kMaxTargets]` (**~L970**). At 64 these are fine on the
stack; at several hundred `TargetProcess`(64 B)/`TargetMetrics`(128 B) they are multi-KB temporaries
per tick — promote to reused `Impl` members. **mitigates BR1.**

---

## Findings — where the real code contradicts / complicates the architecture's assumption

1. **The revert-to-captured-prev "fix" is only safe once the journal exists.** Architecture §3
   frames the F18/F13 MASS bug as "revert-to-captured-prev, not to-all-cores." But the current
   revert-to-all-cores is a **documented deliberate choice** (`ActionExecutor.hpp` **~L138–154**):
   for a single target, restoring a captured mask could propagate a crash residual. The MASS fix
   is correct — but only because S2 adds a crash-surviving journal, which makes captured-prev
   reliable. **Sequencing constraint: S2 (journal) must land with or before S3's revert change**;
   flipping the revert line alone, without the journal, would reintroduce exactly the residual-leak
   the current code guards against. Not a contradiction of the design — a build-ordering constraint
   the architecture's one-line framing hides.

2. **A `1u << i` over-shift is already latent at the current cap (pre-MASS).**
   `AgentRuntime.cpp` `memory_hit_mask` (`uint32_t`, **~L1012/1027/1057**) indexes targets up to
   `kMaxTargets = 64`, so `1u << i` for `i ≥ 32` is undefined behavior **today**. MASS widening
   only makes it worse; the fix (a wider/array mask) is mandatory, not optional. Flagged rather
   than papered over.

3. **`EtwProviderSet` cannot actually split CpuOnly vs Full.** The header
   (`observer/include/phynned/observer/EtwProviderSet.hpp` **~L64–67, ~L152–170**) states
   `MetricsCollector` has no partial-provider disable, so both tiers call the same `start()`.
   The architecture's "burst-only / on-demand Session B" (K3) is honored at the *session* on/off
   granularity (None vs running), not at per-provider granularity. The K3 guarantee holds via the
   None tier; the finer split is deferred (Master Plan §5), not delivered — stated so no one reads
   the tier enum as an implemented capability.

<!-- Made with my soul - Swately <3 -->
