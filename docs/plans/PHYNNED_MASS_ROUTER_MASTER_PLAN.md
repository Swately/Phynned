<!-- PLAN_TIER T1/T2 artifact — the WHY + the binding architecture spine + the phased milestones. -->
<!-- Authored by the build-planning session against the APPROVED-FOR-HANDOFF architecture -->
<!-- (PHYNNED_MASS_ROUTER_ARCHITECTURE.md) and the FROZEN objective (…_OBJECTIVE.md). -->
<!-- FDP applies: verify-before-claim, calibrated status, zero inflation. This plan does NOT -->
<!-- redesign the architecture — it plans the build of it. -->

# Phynned Mass-Router — Master Plan

**Tier: 2** (per PLAN_TIER_PROTOCOL — blast-radius review required). The crash/stability
*and* security triggers all fire: mass process-affinity across hundreds of PIDs, anti-cheat
process boundaries, and a persistent journal-revert path. A Tier-2 change may not be declared
safe or shipped with any risk left `open` in the register.

**Sibling documents (read together — this trio is the plan):**
- HOW / concrete edit sites: [`PHYNNED_MASS_ROUTER_IMPLEMENTATION_STRATEGIES.md`](PHYNNED_MASS_ROUTER_IMPLEMENTATION_STRATEGIES.md)
- Risk register (authored separately by the supervisor): [`PHYNNED_MASS_ROUTER_RISK_REGISTER.md`](PHYNNED_MASS_ROUTER_RISK_REGISTER.md)

**Upstream inputs (frozen / approved):**
- Chosen architecture: [`PHYNNED_MASS_ROUTER_ARCHITECTURE.md`](PHYNNED_MASS_ROUTER_ARCHITECTURE.md) (APPROVED FOR HANDOFF)
- Frozen objective + acceptance metrics: [`PHYNNED_MASS_ROUTER_OBJECTIVE.md`](PHYNNED_MASS_ROUTER_OBJECTIVE.md)
- Evidence base: [`../research/ON_BOX_FACTS.md`](../research/ON_BOX_FACTS.md) + the five `../research/SOTA_*.md` dossiers.

Status: **DRAFT — awaiting operator double-green-light to begin M0.** No code has been
written against this plan.

---

## §1 — Why (the mission, restated as the build contract)

Turn Phynned from a curated per-game pinner into a **mass, automatic, measurement-driven CPU
placement service** for the 7950X3D dual-CCD box: every *touchable* process is placed on the
cores it benefits from — cache-sensitive work toward the V-Cache CCD (CCD0), clock-sensitive
work toward the frequency CCD (CCD1), the background herd penned away from both — at near-zero
agent cost, provably reversible, with every decision explainable and backed by on-box A/B
measurement.

The premise is validated, not assumed: the pointer-chase micro on the operator's silicon
measured a **3.3×** latency step from L3-resident to the 64 MB regime and **13×** to DRAM
([`ON_BOX_FACTS.md`](../research/ON_BOX_FACTS.md) §3, tagged [M]). The thing the router routes
for demonstrably matters on this hardware. What is *not* yet established is a clean per-CCD
delta — the on-box A/B was noisy and inconclusive (ON_BOX §4, [M-noisy]); establishing it on a
quiet box is milestone M1's job, not a claim we may make now.

**The load-bearing safety insight the AAP fan-out surfaced** (architecture §1): modern
anti-cheat is service- or driver-model, so the AC process itself is name-denylistable, but the
*protected game and its legitimate children are not*. A shader-compile worker inside an
EAC/BE/Vanguard-protected game's tree is simultaneously the allowlisted "cache winner" we most
want to route **and** inside a boundary an external SET-handle-opener must never cross. The
chosen architecture (the "C" safety-first spine) is the only candidate in which this hole is
*closable rather than structural* — its default-deny admission gauntlet extends naturally to
"refuse anything in a tree containing a detected AC signature." That refusal is the honest
M2-breadth tax (architecture §5); safety (M1) outranks it under the frozen priority.

## §2 — The design contract (the binding spine — restated from architecture §4)

This layer table is the architecture's, restated here as the **binding build spine**. The build
is conformant iff every placement path lands in exactly one row and honors its mechanism.

| Layer | Membership | Mechanism | Why (binding rationale) |
|---|---|---|---|
| Game | fullscreen / AMD-detected | **untouched** | AMD 3D V-Cache driver owns it; touching its tree is the AC hole |
| Cache-winner | curated ∧ evidence-passed ∧ not-in-AC-tree | **hard affinity → CCD0** | child inheritance is *wanted* (pulls worker children onto V-Cache); names CCD0 only |
| Clock-winner | curated loser-class | **untouched** (falls through) | documented ~−5% on cache CCD → M2c do-no-harm for free |
| Herd | touchable ∧ admitted ∧ low-activity | **soft CPU-Set → CCD1** | cannot starve (soft), lightest right, names CCD1 only → disjoint from CCD0 |
| Denylist / AC-tree / self-managed / PPL | — | **never SET-opened** | the M1 veto — structural, not asserted |

**Two invariants the spine makes structural (not asserted):**
1. **Composability-by-geometry.** Only the hard-affinity mask names CCD0; every soft CPU-Set
   names CCD1. The layers are therefore core-disjoint *by construction*, so the documented
   composability law "hard affinity silently beats CPU Sets" (objective E4) can never make the
   layers fight. Any edit that lets a CPU-Set name a CCD0 core breaks the spine.
2. **Default-deny admission.** Nothing reaches a `Set*`/CPU-Set call unless positively admitted
   by the gauntlet: `denylist → PPL → AC-signature-tree refusal → self-managed-mask exemption →
   launcher/host → audio/foreground → positive class match → coexistence-defer`. The denylist
   check runs **before any `OpenProcess`**. This is the allowlist reframe (objective E6):
   never "touch everything except exceptions."

Measurement is **burst-only, off the latency path**: a birth applies a provisional per-exe
verdict instantly; the A/B arbiter (cycles-per-unit-work via `QueryProcessCycleTime`, plus an
optional *calibrated* PMC accelerator) refines a per-exe evidence store that governs future
admissions. A reconcile janitor (NtQSI bulk snapshot every 2–5 s) is the correctness net that
decouples routing from perfect ETW delivery.

## §3 — Module inventory (reuse / extend / new)

Verified first-hand against the current tree (module roots `observer/ action/ core/ config/
learn/ policy/ bench/ ipc/` + vendored `framework/`; includes are `<phynned/...>` and
`<phyriad/...>` after the identifier rename).

**REUSE as-is (load-bearing base that already exists and fits):**
- `framework/topology` FR-3/FR-9 free functions `phyriad::hw::set_process_affinity /
  set_process_priority / set_thread_affinity` — the hard-affinity lever (Cache-winner row).
- `framework/process` `ProcessMetricsSnapshot` (FR-11 NtQSI bulk, ~80–150 µs @ ~200 proc) and
  `CurrentProcess::self_ppid()`.
- `observer/ProcessObserver` — already captures `parent_pid` per target (feeds the tree, free).
- `core/AgentRuntime` tick-loop scaffold; `core/SelfMonitor` (M3a instrument, in-tree);
  `core/AutoRevertGuard` (do-no-harm monitor); `core/InternalWatchdog`.
- `action/AuditLog` (M4a human-readable trail); `policy/PolicyEngine` + `AutoPolicySelector`.

**EXTEND (9 existing modules changed in place):**
1. `framework/etw/SessionManager` — S0: GUID/mode/return-code repairs.
2. `observer/MetricsCollector` — S0 provider wiring; S4 cycles-per-work; S5 slot-cap widening.
3. `observer/ProcessClassifier` — S1: extend `kSystemNames` + new AC-signature denylist tables.
4. `observer/TargetProcess` — S5: widen `kMaxTargets`.
5. `action/ActionExecutor` — S2/S3: journal hookup + revert-to-captured-prev; S5 table caps.
6. `framework/topology/HardwareTopology` — S3: new CPU-Set / EcoQoS wrappers alongside FR-3/9.
7. `core/AgentRuntime` — S1/S2/S3: wire the gauntlet, journal, and new levers into the tick.
8. `bench/ABRunner` — S4: cycles-per-work A/B + auto-generated per-app report (M4b).
9. `learn/PerGameMemory` — S4: back the per-exe evidence store that gates admission growth.

**NEW (8 modules/units created):**
1. `observer/ProcessTree` — incremental tree from birth-time `ParentPID`; ancestry + descendant walk.
2. `observer/AdmissionGauntlet` — the default-deny gate (the §2 chain).
3. `observer/AcSignatureDetector` — detect EAC/BE/Vanguard service+driver signatures; feeds the AC-tree refusal.
4. `observer/CoexistenceDetector` — detect AMD 3D V-Cache Optimizer / Process Lasso / Game Mode (objective E5); drives defer.
5. `action/PlacementJournal` — on-disk write-ahead `PENDING → APPLIED` record (M1b crash recovery).
6. `tools/phynned-revert` — the external dead-man executable (reverts journaled placements after `taskkill /f` of the agent).
7. `framework/topology/CpuSetControl` — the CPU-Set (`SetProcessDefaultCpuSets`) + EcoQoS soft-lever wrappers (new file within the pillar).
8. `learn/EvidenceStore` — the per-exe A/B evidence record the gauntlet consults for allowlist growth (built on the extended PerGameMemory).

**Bottom line: 8 new modules, 9 existing modules extended.** (Item 6 is a standalone
executable and item 7 a new file inside an already-extended pillar — both counted as new units
because they are new build targets/translation units, not edits to existing ones.)

## §4 — Milestone path (M0 → M4, each with a measurable on-box exit)

Ordering is the frozen priority made operational: **safety is milestone-zero.** Each milestone
has a real number on the real target (objective E8 / AAP A4 — the staged path *is* the substitute
for a time bound).

| M | Name | Measurable on-box exit (the gate) |
|---|---|---|
| **M0** | ETW GUID repair + AC-safety proof | (a) corrected provider GUIDs + system-logger mode + checked `EnableTraceEx2` return → Kernel-Process **birth events actually arrive** (`events_processed() > 0`, non-empty birth stream) on the box; (b) **zero SET-rights handle opens** on a live EAC/BE/Vanguard process **and its entire descendant tree**, proven from `audit.bin` over a real protected-game session (objective M1d). Safety is the veto → this gates everything. |
| **M1** | Clean per-CCD A/B (quiet box) | a reproducible CCD0-vs-CCD1 delta on a real workload, measured in **cycles-per-unit-work** (`QueryProcessCycleTime`), on a quiet box, with variance below the signal — resolving the noisy ON_BOX §4 result. This is the routing signal's existence proof (objective K4). |
| **M2** | Evidence-gated allowlist growth | ≥1 exe promoted into the allowlist **only after** its A/B evidence passed the gate; **M2b** reproduced (≥1 double-digit non-game win — shader-compile / emulator / DB class); **M2a** placement-vs-A/B agreement reported, ≥50% floor. |
| **M3** | Cost soak vs the M3 ceilings | 10-min soak under the §5 streamer + idle-flood mix: SelfMonitor CPU **<0.5% of one core**, RSS **<20 MB** (UI excluded), birth→placement **p95 ≤500 ms** — all logged. The ≥4-hour M1a safety soak + M1c audio-glitch instrument ride on this soak (zero allowlist violations, zero attributable crashes, audio-glitch counter not above agent-off baseline). |
| **M4** | Published A/B report | the extended `bench` **auto-generates a per-app A/B report** (M4b) with every decision carrying a logged human-readable reason (M4a) — publishable per the project's evidence discipline. |

Milestone dependency is strict: **M0 gates all** (no placement flag may default on until M0's
zero-SET-open proof is green). M1's signal is a prerequisite for M2's evidence gate. M3's
ceilings are hard; a design conflict inside them is resolved M2 > M3 (objective §2 priority).

## §5 — Deferred (in-arc, later) vs Out of scope (never, this arc)

**DEFERRED — in scope for the arc, gated to a later milestone:**
- **Burst PMC engine** (`CacheMisses`/MPKI accelerator). Adopt-or-drop is an M0/M1 calibration
  decision: adopt only if it correlates with the cycles-per-work A/B arbiter on this Zen 4 box;
  else drop to A/B-only (GREEN either way — the objective already makes cycles the ground truth,
  PMC an accelerator, ON_BOX §2). LLC-attribution of `CacheMisses` on Zen 4 is unresolved and
  must not become a dependency.
- **Job Objects as a lever** (O(1) group policy). The spine's default herd lever is the soft
  CPU-Set; Job Objects are deferred (membership is one-way — objective E4 — a sharper tool than
  M0 needs).
- **Routing shader-workers spawned *inside* AC-protected trees** — the architecture §5 weakest
  point. Deliberately abstained; may remain out of reach while AC-tree ambiguity holds. Not a
  bug to fix — the accepted M2-breadth tax.
- **Partial-provider ETW sessions** (CpuOnly vs Full split). `EtwProviderSet` already notes
  `MetricsCollector` lacks partial-disable; the tier split is retained but not implemented now.
- **Homogeneous / single-CCD topologies** — degrade to monitor-only (objective E2). Supported,
  not optimized; the 7950X3D is the target that must win first.

**OUT of scope — explicitly not this arc, at any milestone:**
- **Kernel driver / test-signing** — forbidden (objective E1/K5). Elevated user-mode only.
- **Weakening, bypassing, or probing anti-cheat in any way** — forbidden (objective K1).
- **The game-pinning layer as a novel claim** — commoditized; AMD's driver owns it; the Game
  row stays *untouched* by design.
- **Fleet-wide average throughput claims** — the objective (§7) targets outliers/isolation, not
  a fleet average; the build must not market one.
- **Model-weight training / human-out-of-the-loop autonomy / GPU routing** — different arcs.

## §6 — Kill criteria carried into the build (the abort gates)

The objective's kill criteria (§6) remain live during construction — any hit **aborts the flag,
not just fails a test**: K1 (SET-open on an AC process/descendant), K2 (cannot demonstrate
journal recovery after `taskkill /f`), K3 (any component whose *designed* steady-state cost
exceeds the M3 ceilings), K4 (routing signal unobtainable AND no A/B fallback), K5 (needs a
driver), K6 (cannot measure the do-no-harm bound). Each maps to a milestone gate in §4 and to a
mitigation-as-code edit site in the strategies doc, cross-referenced by risk ID in the register.

**Byte-identical-off discipline (binding):** every new mass-placement capability defaults OFF.
With the mass flag off, the agent's observable behavior and output must be **byte-identical** to
the pre-MASS build — the journal writes nothing when no placement is applied, the CPU-Set/EcoQoS
levers are never invoked, and the gauntlet's only effect is refusal. This is what lets M0 ship
the ETW + safety machinery ahead of any behavior change.

<!-- Made with my soul - Swately <3 -->
