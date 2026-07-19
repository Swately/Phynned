<!-- AAP A3/A4/A5 output — the chosen architecture, its synthesis rationale, and the veto verdict. -->
<!-- Selection cites the A2 scorecards (AINV-7), uses the frozen priority (AINV-3), grafts salvage, -->
<!-- names the honest weakest point. This is the APPROVED-FOR-HANDOFF artifact per AAP §5/§9. -->

# Phynned Mass-Router — Chosen Architecture (AAP A3/A4/A5)

Status: **APPROVED FOR HANDOFF** (2026-07-17) · AAP run: the protocol's second independent-domain
instance. Frozen objective: [`PHYNNED_MASS_ROUTER_OBJECTIVE.md`](PHYNNED_MASS_ROUTER_OBJECTIVE.md)
(byte-intact since A0 — AINV-12). Candidates: [`aap_candidates/`](aap_candidates/). Scorecards:
the A2 workflow `wf_b3cda160-6bd` (13 agents: AT1 diversity + 12 adversaries). On-box facts:
[`../research/ON_BOX_FACTS.md`](../research/ON_BOX_FACTS.md).

---

## §1 — The decisive finding: a UNIVERSAL fatal flaw the fan-out surfaced

Every one of the four candidates named a *different* honest weakest point. The 12 clean
adversaries found they share **the same** one — and it lands on **M1, the frozen veto axis**:

| Cand | M1 (veto) | M2 efic | M3 cost | M4 evid | Why M1 fell |
|---|---|---|---|---|---|
| A event-driven | **FAIL** | 2 | 3 | 4 | K1: AC-descendant tree built from a *lossy* ETW birth stream (the janitor exists because births drop under load) → a dropped birth = a missing ancestry node = an AC descendant gets SET-opened. K2: journals AFTER the Set* → taskkill window orphans an applied placement. |
| B measurement-first | **FAIL** | 4 | 3 | 5 | K1: *provisional-apply-at-birth* opens a SET handle on an allowlisted shader/emulator worker spawned **inside** an EAC/BE-protected game's tree, before any descendant-of-AC flag can exist. K2: Job/CPU-Set/EcoQoS revert not in the journal schema. |
| C safety-first | **AT_RISK** | 3 | 5 | 4 | K1 *conditional*: the PPID-ancestry walk can't reach **service-model** AC (EAC/BE run as Windows services, Vanguard as driver+service) — the AC is not in the game's ancestry, so a descendant guard misses it. **Not designed-in; a hole in its own mechanism.** |
| D minimal | **FAIL** | 3 | 4 | 4 | M1a/E6 breach: its herd is a **denylist-complement** ("touch every low-activity process not explicitly denied") — a direct violation of the binding E6/F0 allowlist reframe ("never touch everything except exceptions"). |

**The load-bearing insight (verified against SOTA_SAFETY_COMPAT F9/F11 + the denylist tables):**
modern anti-cheat is **service- or driver-model**, so the AC process itself is name-denylistable
— but the **protected game and its legitimate children** are not, and a shader-compile worker
running inside a protected game's tree is simultaneously (a) exactly the allowlisted "cache
winner" the router wants to route and (b) inside a boundary an external SET-handle-opener must not
cross. No candidate's descendant-tree mechanism closes this, because a PPID walk from the worker
reaches the *game*, not the *AC*. **This is the producer-blindness AAP exists to catch: each
designer saw a different flaw; only the adversarial fan-out saw the shared one.**

## §2 — Selection against the frozen priority (AINV-3/AINV-7)

The frozen ordering is **M1 = veto → M2 > M3 > M4**; §2 states a design failing any M1 item is
"dead regardless of other scores." That rule is decisive and non-taste:

- **B is not the winner despite the best efficacy+evidence (4/5).** M1=FAIL disqualifies it under
  the veto — and worse, its *provisional-apply-at-birth*, the very mechanism that gives it instant
  placement, is what *creates* the AC-descendant hole (it acts before ancestry exists). Its
  strength and its veto-failure are the same feature.
- **A and D are out:** A on the same lossy-tree AC hole + a journal-ordering orphan bug; D on a
  clean **objective violation** (denylist-complement herd vs the binding allowlist reframe).
- **C is the winning spine — the only candidate not outright vetoed (AT_RISK, not FAIL) — and the
  only architecture in which the universal AC flaw is *closable rather than structural*.** C's
  organizing principle is a **default-deny admission gauntlet**: nothing reaches a Set* call unless
  positively admitted. Tightening that gauntlet to refuse "any process inside a tree that contains
  a detected AC service/driver signature" is a *natural extension of C's existing mechanism*, not a
  bolt-on — whereas in A/B/D the hole is structural (A's lossy tree, B's act-before-flag, D's
  touch-everything-not-denied). C also wins M3 outright (5/5, cheapest — the gauntlet is sub-µs
  string compares over an already-held snapshot) and carries the soundest M1b answer (write-ahead
  PENDING→APPLIED journal + external dead-man).

**Chosen: the C (safety-first) spine, grafted with the survivors' salvage.**

## §3 — The grafts (salvage from the runners-up — AINV, no-deletion)

C's own weakest point (its designer's honest disclosure, confirmed by the efficacy lens at M2=3):
refusing to route arbitrary processes reduces it toward "curated list + soft pen," close to what
Process Lasso + the AMD driver already do — thin M2 **breadth**, not accuracy. The grafts target
exactly that, without weakening the safety spine:

- **From B (the niche) — measured benefit as the LIST-GROWTH mechanism.** C already wants its
  allowlist to "grow only by on-box A/B evidence." Graft B's measurement engine *as that evidence
  gate*: cycles-per-unit-work via `QueryProcessCycleTime` (the ON_BOX §4-vindicated arbiter, immune
  to the clock confound) is the always-available signal; the burst PMC engine is an
  **accelerator-that-must-earn-trust** in an M0 calibration milestone (adopt if it correlates with
  the A/B arbiter on this Zen 4 box, else drop to A/B-only — GREEN either way, K4 satisfied by
  construction). This gives C's safety with B's differentiator, and it stays inside the allowlist.
- **From A — three cheap structural wins:** (1) birth-time `ParentPID` → *incremental* ProcessTree
  at zero query cost, which the descendant-refusal gate needs; (2) **composability-by-geometry** —
  only the hard-affinity mask names CCD0, every soft CPU-Set names CCD1 → core-disjoint by
  construction, so "hard affinity beats CPU Sets" can never make the layers fight; (3) the
  reconcile janitor as the correctness net decoupling routing from perfect ETW delivery; (4)
  on-demand Session B (CSwitch/PMC) gated behind an active A/B only, stopped after → K3 satisfied.
- **From D — the cheap-and-safe defaults:** CPU-Set via `PROCESS_SET_LIMITED_INFORMATION` (cannot
  fail, lightest right, avoids the `PROCESS_TERMINATE` bit that AV/EDR heuristics flag) as the
  default mass lever; **deliberately NOT curating the documented losers** (ordinary compile,
  Blender, ffmpeg) so they fall to "untouched" → M2c do-no-harm for free; the self-managed-mask
  exemption; denylist-check BEFORE any `OpenProcess`.
- **C's own load-bearing mechanisms kept:** the write-ahead journal (revert-to-captured-prev, not
  to-all-cores — fixes the F18/F13 MASS bug), the external `phynned-revert.exe` dead-man for
  kill-without-restart, the CoexistenceDetector + defer (no Forced-Mode) against optimizer-flapping.

## §4 — The chosen architecture (one paragraph + the layer table)

A **default-deny, evidence-gated, provably-reversible router**. The corrected ETW
Kernel-Process birth stream (the four GUID/mode/return-code repairs are prerequisite M0 work) feeds
an **incremental ProcessTree** (from `ParentPID`, free). Every candidate PID passes the **admission
gauntlet** — denylist → PPL → **AC-signature-tree refusal** (denylist the AC service/driver AND
refuse anything in a tree containing one) → self-managed-mask exemption → launcher/host → audio/
foreground → positive class match → coexistence-defer — and only an admitted PID reaches a Set*
call, write-ahead-journaled PENDING→APPLIED. Placement layers:

| Layer | Membership | Mechanism | Why |
|---|---|---|---|
| Game | fullscreen/AMD-detected | **untouched** | AMD driver owns it; touching its tree = the AC hole |
| Cache-winner | curated ∧ evidence-passed ∧ not-in-AC-tree | **hard affinity → CCD0** | inheritance *wanted* (pulls worker children onto V-Cache); names CCD0 only |
| Clock-winner | curated loser-class | **untouched** (falls through) | documented −5% on cache CCD → M2c for free |
| Herd | touchable ∧ admitted ∧ low-activity | **soft CPU-Set → CCD1** | cannot starve (M2c), lightest right, names CCD1 only → disjoint from CCD0 |
| Denylist / AC-tree / self-managed / PPL | — | **never SET-opened** | the M1 veto, structural not asserted |

Measurement is **burst-only, off the latency path**: birth applies a provisional per-exe verdict
instantly; the A/B arbiter (+ optional calibrated PMC) refines the per-exe evidence store that
governs future admissions. Reconcile janitor (NtQSI every 2–5 s) is the correctness net.

## §5 — The chosen design's single honest weakest point (AINV, mandatory)

> **AMENDED 2026-07-17 (post-AAP operator refinement — see the objective's re-open note + risk
> register CR1).** The AC-safety approach below (conservative abstention) was the AAP-selected
> C-spine. The operator's try-then-back-off directive + the anti-cheat research
> ([`../research/SOTA_ANTICHEAT_AFFINITY.md`](../research/SOTA_ANTICHEAT_AFFINITY.md)) replaced it
> with **zero-handle AC-driver detection + a least-privilege single cached probe**. This SHRINKS
> the weakest point stated below: the router now *probes* (a)/(b) titles instead of abstaining, so
> the M2-breadth tax falls to only the (c)/unrecognized-AC residual (do-not-probe, fail-safe). The
> original text is kept as the AAP record; the CR1/S1 docs carry the live design.

**Conservative abstention caps M2 breadth exactly where efficacy was highest.** The AC-safety fix
is "when a process sits inside a tree touched by anti-cheat, refuse to route it." But the richest
cache-winner case — shader-compile workers spawned *inside* AC-protected game trees — is precisely
what gets refused. So the product routes the compute it can *prove* safe (standalone shader
compiles, emulators, databases, the herd) and **abstains inside AC-ambiguity**. That is the honest
M2-breadth tax for clearing the M1 veto; it is the same tension C's designer named, now made
explicit and accepted. The router is honest-but-conservative, not omnipotent — and on the frozen
priority (M1 > M2), that is the correct trade, not a defect to hide.

## §6 — A4 feasibility veto (the "not blind" gate)

Each kill criterion, recomputed against the chosen design + ON_BOX facts:

| K | Cleared? | Basis / gating milestone |
|---|---|---|
| K1 AC descendants | **CLEARS (conditionally, milestone-gated)** | denylist-first + AC-signature-tree refusal + abstention. **M0-safety milestone (the FIRST milestone — safety is the veto): prove zero SET-opens on a real EAC/BE/Vanguard tree on the box.** |
| K2 journal revert | CLEARS | C's write-ahead journal + dead-man is the soundest of the four; milestone: `taskkill /f` A/B on the box. |
| K3 no always-on full-rate trace | CLEARS | burst-only measurement (grafted from A/D). |
| K4 signal obtainable | CLEARS | cycles-per-work A/B always available (ON_BOX §4); PMC an accelerator, not a dependency. |
| K5 no driver | CLEARS | user-mode ETW + CPU-Sets + affinity only. |
| K6 do-no-harm measurable | CLEARS | AutoRevertGuard + softest-lever + loser-classes-untouched; M2c A/B measured. |
| K7 costume diversity | CLEARS | AT1 verdict DIVERSE (four distinct organizing bets + control mechanisms). |

**Verdict: CLEARS → APPROVED FOR HANDOFF**, with the staged, on-box-measurable milestone path
M0(ETW-repair + AC-safety proof) → M1(clean per-CCD A/B on a quiet box, cycles metric) →
M2(evidence-gated list growth) → M3(cost soak) → M4(published A/B report). Every milestone has a
real number on the real target (E8/A4 satisfied). Safety (M1) is milestone-zero — correct under the
veto ordering.

## §7 — Handoff (A5) + the AAP's own second-instance verdict

- The chosen design becomes a frozen identity; its **documentation** enters KAP; its
  **implementation** enters PLAN_TIER — presumptively **Tier-2** (crash/stability/security triggers
  all fire: mass affinity, AC boundaries, journal-revert), so it re-enters the RISK_REGISTER gate on
  its own terms.
- **AAP self-verdict (its §0.1 second-instance requirement):** this run produced a selection its
  own designers could not have reached alone — the fan-out surfaced a *shared* fatal flaw (AC
  descendants) that every isolated designer missed while each named a different weakest point. That
  is the producer-blindness the protocol is built to defeat, demonstrated on a software-systems
  problem (independent of the first FPGA instance). Whether AAP-selected beats ad-hoc-chosen remains
  `value-indeterminate` pending the operator's decision-equivalence judgment — but the run was not
  vacuous: it changed the answer.

<!-- Made with my soul - Swately <3 -->
