<!-- Tier-2 RISK_REGISTER for the Phynned mass-router. The pre-commit gate: nothing ships while -->
<!-- a row is `open`. Each risk = failure mode (file:line) + mitigation-as-code + first-hand -->
<!-- verification + terminal status. Authored by the supervisor (the safety-critical artifact). -->

# Phynned Mass-Router — Risk Register (Tier-2)

Status: `designed` — every row `open` (nothing built). Tier **2** per
[`PLAN_TIER_PROTOCOL`](../../../../protocols/quality/PLAN_TIER_PROTOCOL.md) §1.1: triggers 1
(crash/hang — mass affinity + ETW session), 2 (concurrency — cross-process state), 3 (security —
elevated mass handle-opening across a live system, anti-cheat boundaries), 4 (irreversibility —
placements persist after the setter dies). Siblings (the three are one set):
[`PHYNNED_MASS_ROUTER_MASTER_PLAN.md`](PHYNNED_MASS_ROUTER_MASTER_PLAN.md) ·
[`PHYNNED_MASS_ROUTER_IMPLEMENTATION_STRATEGIES.md`](PHYNNED_MASS_ROUTER_IMPLEMENTATION_STRATEGIES.md)
(cites each risk ID at its edit site) · design origin:
[`PHYNNED_MASS_ROUTER_ARCHITECTURE.md`](PHYNNED_MASS_ROUTER_ARCHITECTURE.md).

**The gate (PLAN_TIER §3):** no code lands, and no milestone is claimed complete, while any risk
below is `open`. `mitigated` requires the Verification column to have been **run first-hand on the
box** (verify-before-claim binds hardest on the safety-veto artifact), not assumed. `accepted`
requires a written residual + the operator's sign-off. Risks discovered during the build are added
here — the register is living until the whole arc ships.

**Provenance:** these rows are the AAP A2 fatal-flaw findings (workflow `wf_b3cda160-6bd`) plus the
[V]-tagged incidents in [`../research/SOTA_SAFETY_COMPAT.md`](../research/SOTA_SAFETY_COMPAT.md).
Ordered by blast radius.

---

### `CR1` — anti-cheat handle-open (the universal fatal flaw; the M1 veto)

| Field | Content |
|---|---|
| **Class** | Security (K1) — **the flaw that vetoed 3 of 4 AAP candidates**; mitigation redesigned 2026-07-17 on the operator's try-then-back-off directive + the [V]-tagged evidence in [`../research/SOTA_ANTICHEAT_AFFINITY.md`](../research/SOTA_ANTICHEAT_AFFINITY.md) |
| **Failure mode** | Opening a `PROCESS_SET_INFORMATION` handle on a process inside an anti-cheat boundary. Detection is at **`OpenProcess`/handle-open** (AC `ObRegisterCallbacks` edits `DesiredAccess` before the handle exists) — NOT at the affinity call, so the *open itself* is what the AC observes. Behaviour splits by AC class: **(a) CLEAN-BLOCK** (EAC — Halo MCC/Elden Ring: `ACCESS_DENIED`, catchable, no memory touched); **(b) ALLOW** (Vanguard/LoL, VAC, base BattlEye — affinity succeeds, no punishment; the operator's LoL observation); **(c) SILENT-THEN-PUNISH** (Ricochet/CoD, kernel-BattlEye/Destiny 2 — the open **succeeds locally with no error** and is *reported*; punishment is a remote kick/flag). The (c) class is the killer: a "try then back off on error" loop **cannot fire** — there is no local error to catch. |
| **Mitigation (as code)** | A **zero-handle AC-detection gate BEFORE any `OpenProcess`**, then a least-privilege cached probe. **Step 1 — zero-handle detection (opens NO handle on the game):** enumerate loaded kernel drivers + running services system-wide (`Win32_SystemDriver`/`EnumDeviceDrivers` + SCM — confirmed on-box 2026-07-17: 182 drivers / 116 services visible with zero handle on any process) and read the game's image name/path from the ETW birth event. The **AC driver identity is the class oracle:** `vgk.sys`→Vanguard→(b); EAC driver→(a); Ricochet/kernel-BattlEye driver→(c). **Step 2 — classify → decide:** no AC detected, or a known **(a)/(b)** driver → run the probe; a known **(c)** driver, OR an **unrecognized AC driver active on the foreground game** → **do-not-probe** (fail-safe: unknown class = never open a handle on the title). **Step 3 — least-privilege single cached probe** (the operator's try-then-back-off, for the probe-allowed set): `OpenProcess(0x1200 = PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION)` — never `VM_READ`/`VM_WRITE`/`PROCESS_QUERY_INFORMATION`/`ALL_ACCESS`, no cheat shape (QUERY_LIMITED is required to read the affinity mask for the no-op set + the DR1 revert prev; SET-only fails `GetProcessAffinityMask` — verified on-box 2026-07-17; the probe uses the same handle the real router uses); attempt the placement **once**; on `ACCESS_DENIED` (the (a) signal) mark the exe **BLOCKED** in `learn/EvidenceStore` (`LearnedEntry::ac_probe`) and **never re-probe**; on success mark **ALLOWED** and route. The per-title label is permanent (one probe per exe-identity, ever). This retires the hardcoded (c) exe list in favour of runtime driver-detection; a small static driver→class map (the [V]-tagged classifications) seeds the oracle, everything unmapped falls to the conservative default. |
| **Verification** | **M0-safety A/B RAN on the operator's box 2026-07-17 — MECHANISM PROVEN on real anti-cheat:** (1a) **no-AC (Halo MCC, EAC OFF):** probe opened 0x1200, read mask 0xffffffff, no-op set **succeeded** → ALLOWED, opens=1, affinity unchanged. (1b) **(a) EAC (Halo MCC, EAC ON):** probe opened 0x1200, GetAffinity succeeded, `SetProcessAffinityMask` **DENIED** → BLOCKED, opens=1, affinity unchanged, **operator confirmed no crash / no kick** (empirical clean-block). Mechanism refinement (verified vs `AcProbe.cpp` + audit): this EAC blocks at the SET *call*, not at `OpenProcess` — the SET\|QUERY handle opened, only the set was denied (refines the research's "detect-at-open"; harmless, no memory touched). (2) **(b) Vanguard (LoL, `League of Legends.exe` — the game, not the CEF client processes):** probe opened 0x1200, no-op set **succeeded** → ALLOWED, opens=1, affinity unchanged, **operator confirmed in-game no crash / no Vanguard warning**. (3) **(c) do-not-probe (`bf6.exe`):** Refused, **ZERO handle opens** (audit empty). **All four AC behaviour classes verified on real games**, and a design-validating finding: the two most aggressive kernel ACs behave OPPOSITE on affinity (EAC blocks the set, Vanguard allows it) — proving per-AC classification is necessary, not a universal rule. **S3 LIVE INTEGRATION DONE + VERIFIED 2026-07-17:** the AC-gate is now live in `AgentRuntime` (`core/src/AgentRuntime.cpp:1129-1143`) — for a `kind==Game` target it calls `AcProbe::probe_and_label` and, on Refused/Blocked, logs `[CR1] AC-gate SKIP` + `continue`, **before** both `executor.apply` (L1182) and `apply_differential_pin` (L1166) — verified by grep + read: no apply path bypasses it. **Still `open`:** the M1a live-desktop soak (the agent running against real AC games over a session, asserting zero handle opens on a (c) title live), and the descendant-tree case (a non-Game compute process inside an AC game's tree — the gate currently keys on `kind==Game`; the herd/descendant handling is M1+). |
| **Status** | `open` — the K1/M1d amendment this rests on is **SIGNED by the operator (2026-07-17)** and applied to the frozen objective. **Build in progress:** `AcDriverOracle` (zero-handle detection) built + its test proved the surface (227 drivers / 773 services readable, no game handle). **On-box finding that refined the design (verified first-hand 2026-07-17):** a box-wide AC-driver fold is the WRONG gate — (1) installed-but-**stopped** AC services (`EAAntiCheatService`/`EasyAntiCheat_EOS`/`vgc` all Stopped here) must not count; (2) Vanguard's `vgk` kernel driver is **always Running** (boot-resident) even with no Riot game open, so driver-presence ≠ game-protected. **Correction:** detection is now **game-specific by exe identity (known-AC-title→class map, primary)** + running-AC-service enum (SERVICE_ACTIVE, always-on drivers like `vgk` flagged) as a secondary for unknown titles only. `observer/AcDriverOracle` BUILT + VERIFIED first-hand 2026-07-17 (test compiles `-Wall -Wextra`, passes; the operator's 4 games classify deterministically — Halo MCC→CleanBlock_A, LoL→Allow_B, BF6→UnknownAc [`bf6.exe` exe-name confirmed live], Fortnite→CleanBlock_A; +CoD→SilentPunish_C, Destiny→SilentPunish_C). Stays `open` until the least-privilege probe (S1b) is built and the three-part probe test runs on the box. |

### `CR2` — ETW session crash / zombie (L3 crash-persistent resource)

| Field | Content |
|---|---|
| **Class** | Crash / irreversibility (SAFETY_PROTOCOL §5 L3) |
| **Failure mode** | The corrected ETW real-time session (Kernel-Process birth + on-demand system-logger CSwitch/PMC) is a crash-persistent resource: a crash inside the acquire→release window leaves a zombie kernel session that outlives the process (killing the agent does not free it; recovery needs `logman stop` or reboot). Compounded by the **existing defect** the event dossier found: `framework/etw/include/phyriad/etw/SessionManager.hpp:~160` uses a wrong provider GUID and `~171` uses StackWalkGuid, and `SessionManager::start` **ignores the `EnableTraceEx2` return code** → the session can fail silently and a half-open session can zombie. |
| **Mitigation (as code)** | (1) Repair the GUIDs to the real `Microsoft-Windows-Kernel-Process {22FB2CD6-...}` (kw 0x10) + the System-Scheduler CSwitch under `EVENT_TRACE_SYSTEM_LOGGER_MODE`, and **check every `EnableTraceEx2`/`StartTrace` return** (S0). (2) A named session with a **pre-registration + reclaim-on-start**: the agent tries to stop any prior session of its known name at startup (`ControlTrace(..., EVENT_TRACE_CONTROL_STOP)`) before opening a fresh one — a zombie from a prior crash is reclaimed, not leaked. (3) The L3 gate (§5): the open/close pair is named in the code, the close is reachable from every exit including the crash path (the reclaim-on-start IS the crash-path recovery). |
| **Verification** | On the box: start the agent, `taskkill /f` it mid-session, confirm via `logman query -ets` that the named session is either gone or reclaimed clean on next start (no accumulation across 5 kill/restart cycles). The GUID repair is verified by birth events actually arriving. |
| **Status** | `open` — **GUID repair BUILT + VERIFIED on the box 2026-07-17** (`SessionManager.hpp:159` → real `{22FB2CD6-...}` verified via `logman query providers`; `providers_enabled`/return-code capture added). Standalone `etw_birth_test`: `providers_enabled=1`, **10 ProcessStart events** delivered on spawning 5 processes (was structurally 0 before the fix), clean session teardown, zero zombie. **Still `open`:** the 5×`taskkill` zombie-reclaim cycle test, and the deferred CSwitch/`SYSTEM_LOGGER_MODE` part (the wrong `kKernelContextSwitch`=StackWalk GUID, now comment-flagged, belongs to the burst-PMC path M1+, not M0). |

### `DR1` — journaled placement must survive taskkill of the agent (M1b)

| Field | Content |
|---|---|
| **Class** | Irreversibility / data-loss (K2, M1b) |
| **Failure mode** | Affinity/CPU-Set placements are properties of the **target** process and **persist after the setter dies** ([V1] SOTA_SAFETY_COMPAT F17). The current `~ActionExecutor → revert_all` RAII does NOT run on `taskkill /f`/crash → a mass run orphans hundreds of placements. Ordering bug from candidate A's review: journaling AFTER the `Set*` leaves a window where a kill orphans an applied-but-unrecorded placement. |
| **Mitigation (as code)** | A **write-ahead** `action/RevertJournal` (candidate C's spine): status `PENDING` written + `fflush`ed **BEFORE** the `Set*`, flipped to `APPLIED` after; each record keyed on **exe-identity + process creation-time** (not raw PID — survives PID recycle) and stores the **captured prior mask**. `recover_journal()` runs FIRST on agent start, before the tick loop, reconciling each journaled PID against the live NtQSI snapshot and reverting stragglers. An external dormant `phynned-revert.exe` **dead-man** covers kill-WITHOUT-restart (the case on-restart recovery cannot reach). **BUILD-ORDERING CONSTRAINT (verified first-hand 2026-07-17):** the current revert-to-all-cores is NOT a bug to blindly replace — `ActionExecutor.hpp:138-154` documents it as a *deliberate* choice to avoid crash-residual drift (a prior crashed instance leaves a residual narrow mask that a naive captured-prev revert would restore as "default", drifting narrower each crash cycle). Revert-to-captured-prev is therefore only safe **once the journal authoritatively distinguishes "my own prior placement" from "a crashed instance's residual"** — so the journal (S2) MUST land with or before the revert change (S3), never after. Until then the all-cores revert stays. |
| **Verification** | On the box: place N processes, `taskkill /f` the agent, restart → assert 100% of journaled placements reverted to their captured prior (compare pre/post masks). Then the dead-man path: kill without restart → the dead-man reverts within its heartbeat window. |
| **Status** | `open` — **journal + dead-man BUILT + VERIFIED first-hand 2026-07-17** (`action/RevertJournal` + `tools/phynned-revert`). `revert_journal_test` green (write-ahead durability proven: a 2nd reader sees the PENDING record on disk before mark_applied; crash-survival recovers 2 APPLIED + 1 PENDING with exact prev_masks; pid-recycle guard drops creation-time mismatches). `phynned-revert --self-test` did the real cycle: narrowed a child's affinity → journaled APPLIED → **reverted to captured prev, verified by re-read** → pass-2 idempotent. **S3 LIVE INTEGRATION DONE + VERIFIED 2026-07-17:** the journal is wired into `ActionExecutor::apply` — write-ahead ordering confirmed (`ActionExecutor.cpp:166` `get_process_affinity` captures prev → `:188` `record_pending` flushes → `:191` `set_process_affinity` → `:197` `mark_applied`); `recover()` runs in the constructor before the tick loop and reverts survivors to captured prev; `revert()`/`revert_all()` now restore captured prev (the all-cores rationale is superseded by recover-on-start, invariant updated). `journal_taskkill_test` **PASSES** first-hand: apply→orphan (leaked executor = taskkill)→fresh construction `recover()`→child restored to captured prev 0x3 (not default, not 0x1). **Still `open`:** the full live-agent `taskkill /f` soak (the harness proves the mechanism; the live-desktop kill adds final confidence); differential-pin (Rule 7) is deliberately NOT journaled (reverts to default — documented, avoids the drift the old comment warned of). |

### `OR1` — oscillation / optimizer-fight (thrash)

| Field | Content |
|---|---|
| **Class** | Concurrency / stability (E5, [V1] F15/F16) |
| **Failure mode** | (a) Consumption-reactive placement changes consumption → re-placement thrash. (b) Phynned + the AMD 3D V-Cache driver + Process Lasso + Windows Game Mode all reassert affinity → flapping. The current `AdaptiveTick` at 100 ms is far too fast to be the placement cadence. |
| **Mitigation (as code)** | (1) Placement is **one-shot per (exe-identity)**, journaled "already-placed" → steady-state re-placement rate ≈ 0 (the delta-driven engine, grafted). (2) A hysteresis/cooldown sub-clock decoupled from the 100 ms tick: a process re-evaluated no more than once per cooldown window; a per-exe move counter that stops re-moving after K flips (borrows the confidence-gate pattern already in `MetricsCollector`'s hot-thread machinery). (3) A `CoexistenceDetector`: if the AMD driver / Process Lasso / Game Mode is detected acting on a PID, **defer** (never Forced-Mode re-apply). |
| **Verification** | On the box: a soak with Process Lasso + the AMD driver active; assert the audit log shows no PID re-placed more than the cooldown allows, and zero placement flaps on a PID another optimizer owns. |
| **Status** | `open` |

### `SR1` — mass handle-opening trips AV/EDR heuristics (the unsigned-elevated surface)

| Field | Content |
|---|---|
| **Class** | Security (detection) ([V3] F19) |
| **Failure mode** | An unsigned, elevated binary opening `OpenProcess` handles across hundreds of processes matches the "unusual handle pattern" archetype (the LSASS-dumper shape) → AV/EDR flags or throttles it. |
| **Mitigation (as code)** | **Least-privilege, never-greedy handles:** the classify path uses `PROCESS_QUERY_LIMITED_INFORMATION` (0x1000) only; the act path uses `PROCESS_SET_LIMITED_INFORMATION` for the CPU-Set mask (the lightest right that works — [V1] SOTA_CONTROL_PLANE #18) or `PROCESS_SET_INFORMATION` for hard affinity, **never** `PROCESS_ALL_ACCESS`/`PROCESS_VM_READ` in the sweep; `bInheritHandles=FALSE`; handles closed immediately after each op (no handle cache in the sweep). Denylist-first means AC/EDR processes are never even opened (composes with CR1). |
| **Verification** | On the box with the operator's real AV/EDR active: a full soak with zero AV/EDR block/quarantine events against the agent; a grep-invariant that `PROCESS_ALL_ACCESS`/`PROCESS_VM_READ` appear nowhere in the mass path. |
| **Status** | `open` |

### `PR1` — clobbering a process's self-chosen affinity (self-managed apps)

| Field | Content |
|---|---|
| **Class** | Crash / do-no-harm (M2c, [V2] F12/F13) |
| **Failure mode** | Apps that size thread pools off processor count (.NET pre-6, Java ForkJoinPool, native `GetSystemInfo`) or manage their own affinity (SQL Server) either deadlock/priority-invert when pinned into fewer cores, or fight the agent. The current base's revert-to-all-cores would also *widen* an app's self-narrowed mask. |
| **Mitigation (as code)** | **Self-managed-mask exemption** (grafted from D): any process whose observed affinity at admission time is **not** the system-default full mask is treated as self-managing → never touched. Plus: the documented loser-classes (ordinary compile, Blender, ffmpeg) are **deliberately not curated** → they fall to "untouched" (M2c for free). The herd lever is a **soft CPU-Set** (cannot starve a process of cores) not a hard mask. |
| **Verification** | On the box: run SQL Server / a `-j32` compile / a .NET app under the agent; assert none has its self-chosen mask altered (audit log) and none regresses >2% vs Windows-default (the M2c A/B, cycles-per-work). |
| **Status** | `open` |

### `BR1` — the MASS-scale integer ceilings (silent truncation)

| Field | Content |
|---|---|
| **Class** | Correctness (blocks the MASS arc) |
| **Failure mode** | Three fixed-width-vs-scale defects, all verified first-hand 2026-07-17: (1) `TargetProcess.hpp` `kMaxTargets=64`; (2) the `MetricsCollector` `pid_hash` slot index is `uint8_t` → a hard ~254-PID ceiling ([V1] event dossier F-5); (3) **a latent UB bug present NOW, pre-MASS:** `AgentRuntime.cpp:1012` `memory_hit_mask` is `uint32_t` but `1u << i` (`:1027`, `:1057`) iterates targets to `kMaxTargets=64` — for i≥32 the shift is undefined behavior (on x86 it wraps mod 32, so target 32 aliases target 0's bit → the memory-hit/AutoRevertGuard logic is already silently wrong for targets 32-63). At 300-800 tracked processes (1) and (2) silently truncate and (3) corrupts across the whole table. |
| **Mitigation (as code)** | Widen `pid_hash` slot to `uint16_t`, `kMaxTargets` to a MASS value (≥1024) with the SHM layout (`AyamaShmLayout`→`PhynnedShmLayout`) re-sized accordingly; **replace the `uint32_t` `memory_hit_mask` bitmask with a per-target `bool`/bitset sized to `kMaxTargets`** (the `1u << i` pattern cannot survive any widening — it must go, not grow to `1ull`, since 64 already exceeds a `uint64_t` at the MASS cap); and the slim ~48-56 B/PID process-level state (fat per-thread state reserved only for the dozens of actively-routed candidates — the event dossier's ~95% memory cut) so RSS stays under M3b (<20 MB). |
| **Verification** | On the box: drive the agent past 500 tracked processes; assert no truncation (every enumerated touchable PID appears in the table), correct memory-hit accounting for targets ≥32 (a targeted unit test), and RSS < 20 MB. |
| **Status** | `open` — **the UB (defect 3) is FIXED 2026-07-17** (`AgentRuntime.cpp:1024` `memory_hit_mask` is now `std::bitset<kMaxTargets>`, not `uint32_t`; `.set(i)`/`.none()`/`.test(i)` replace the `1u<<i` UB; build green, core/action/observer tests 100%). **Still `open`:** the scale-widening (`pid_hash` uint8→uint16, `kMaxTargets` 64→≥1024, SHM re-layout) — deliberately DEFERRED to the mass-routing build (M1+) where it is actually needed and can be verified against the agent↔UI SHM contract, not changed blind. |

---

## Summary

| ID | Class | Blast radius | Status |
|---|---|---|---|
| `CR1` | security — AC handle-open, zero-handle detect + cached probe (the M1 veto) | **catastrophic** (kick/flag + the whole product's trust) | `open` (K1 amendment SIGNED; awaits M0 build) |
| `CR2` | crash — ETW zombie + the existing GUID defect | high (L3, unrecoverable-until-reboot) | `open` |
| `DR1` | irreversibility — journal-revert survives kill | high (mass orphaned placements) | `open` |
| `OR1` | concurrency — oscillation / optimizer-fight | medium | `open` |
| `SR1` | security — mass-handle AV/EDR detection | medium | `open` |
| `PR1` | crash — self-managed-app clobber | medium | `open` |
| `BR1` | correctness — MASS integer ceilings | medium (blocks the arc) | `open` |

**7 `open`, 0 mitigated, 0 accepted.** Nothing builds-to-ship until each is `mitigated` (verified
on the box) or `accepted` (residual + operator sign-off). The milestone ordering is safety-first by
construction: **M0 clears CR1 + CR2 + DR1** (the veto-class trio) before any efficacy milestone
runs — the frozen priority M1-veto made concrete as a build gate.

<!-- Made with my soul - Swately <3 -->
