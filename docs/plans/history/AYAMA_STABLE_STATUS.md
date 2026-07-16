# AYAMA_STABLE_STATUS — what "stable Ayama 0.1.0-experimental" means and how far we are

> **Status:** Initiative-tracker. Opened 2026-05-23 by the
> `cross-session greeting` session per the operator's directive
> *"necesito que se coordinen para que implementen una version estable
> de Ayama el orquestador interactivo"*. **Honest framing:** this doc
> tracks the gap between the working Ayama codebase and a release the
> project would call "stable" under the `0.1.0-experimental` track —
> not a v1.0 maturity claim (v1.x was retracted; see
> [`CANON.md`](../../../CANON.md)).
> **Owner.** Swately (operator).
> **Coordinating sessions.** `cross-session greeting` (this doc + README
> cleanup), `arch P2 — L2b Blackboard` (no overlap), `myriarch + meta
> infra` (no overlap on `apps/ayama/`), `operator-maid bridge` (no
> overlap).
> **Doc lifecycle.** This file lives in `docs/planning/ongoing/` until
> Ayama graduates to a tagged release, at which point it moves to
> `docs/planning/archive/` with a banner.

---

## §1 — Why this doc exists

Ayama is the showcase application for Phyriad and the project's most
mature consumer of the pillar stack. It already ships:

- An agent + UI + CLI + bench tool, six unit-test files, a 14-game
  empirical validation catalogue with 95 % CI, and a non-trivial
  distribution + Defender-exclusion + service-installer pipeline (see
  [`apps/ayama/README.md`](../../../apps/ayama/README.md) and
  [`docs/ayama/reports/EMPIRICAL_EVIDENCE_SUMMARY.md`](../../ayama/reports/EMPIRICAL_EVIDENCE_SUMMARY.md)).

What it does NOT have today is a single doc that says **what would
make this `0.1.0-experimental` release "stable"** and **how close the
working tree is to that bar**. This tracker fills that gap so that
multiple LLM sessions and the human operator can converge on the same
definition of done without re-deriving it each turn.

The doc is intentionally short. Detail per gate lives in the linked
sub-documents, not here.

---

## §2 — Definition of "stable" (gates)

A stable Ayama `0.1.0-experimental` release must satisfy ALL of the
following. The list is deliberately concrete — every line either
green-lights with a measurable check or stays red until a specific
action lands.

| # | Gate | Check | Status |
|---|------|-------|--------|
| **G1** | `phyriad-doctor` green | `bash scripts/phyriad-doctor.sh` returns "all checks passed" | ✅ **Done** — verified 2026-05-23 by `cross-session greeting` session (6/6 checks OK). |
| **G2** | All Ayama unit tests pass on Linux | `ctest --output-on-failure -L ayama` in a fresh build | ❓ **Unverified this initiative** — tests exist (6 files: `action_executor_test`, `abrunner_test`, `baseline_test`, `perceptual_test`, `agent_idle_budget_test`, `agent_runtime_test`); CI passes for `ci-linux.yml`; no fresh local run captured in this tracker. |
| **G3** | All Ayama unit tests pass on Windows | Same `ctest` on Windows MinGW build | ❓ **Unverified this initiative** — no Windows CI workflow exists in `.github/workflows/`; the operator builds locally. |
| **G4** | Full distribution build green | `cmake -DPHYRIAD_BUILD_AYAMA=ON -DAYAMA_BUILD_UI=ON …`, then `cmake --build`; `build/ayama-dist/ayama-ui.exe` exists | ❓ **Unverified this initiative** — recent commits indicate the dist layout works (`f2cf359 ayama: zero-DLL distribution + Defender exclusion script`), but no fresh build was run as part of opening this tracker. |
| **G5** | No retracted-version framing in `apps/ayama/` user-facing docs | `grep -rE 'v1\.0\|v1\.1\|version 1\.0\|1\.0 milestone' apps/ayama/README.md` returns nothing OR returns only inline `(v1.0)` layout-annotation comments in `.hpp` (not maturity claims) | ✅ **Done** — `apps/ayama/README.md` cleaned in this commit (4 edits: lines 90, 133, 199, 233). The 4 inline `(v1.0)` references in `.hpp` files (`AyamaAppState.hpp`, `advanced_panel.hpp`, `AyamaLogicNode.hpp`, `AyamaProtocol.hpp`) are layout-annotation comments — they describe the shape of the SHM publication format, not a maturity claim — and are out of scope for this gate. |
| **G6** | INVENTORY parity for Ayama | `python scripts/check_inventory.py` green for the `apps/ayama/` subtree | ✅ **Done** — covered by G1 (phyriad-doctor runs check_inventory). |
| **G7** | Empirical evidence summary up to date | `docs/ayama/reports/EMPIRICAL_EVIDENCE_SUMMARY.md` reflects the 14 reports linked from the README | ❓ **Unverified this initiative** — the doc exists; no diff check against the README's table this turn. |
| **G8** | License consistency | Root `LICENSE` + `apps/ayama/README.md` + per-file headers reference PolyForm Noncommercial 1.0.0 | ✅ **Done** — the migration landed in commit `b75caca license: Apache 2.0 -> PolyForm Noncommercial 1.0.0`. The README already cites PolyForm NC 1.0.0 in the License section. |

**Aggregate.** 4/8 verified green; 4/8 unverified (need fresh build +
CI run on Win + Linux). No gate is known-failed.

---

## §3 — Per-area state

The current working tree for `apps/ayama/`:

| Subdir | Role | Last touched (commit subject excerpt) | Comment |
|--------|------|---------------------------------------|---------|
| `core/` | `AgentRuntime`, `SelfMonitor`, `AdaptiveTick`, `AutoRevertGuard`, watchdogs | `d0dc489 fix(build): expose version.hpp include paths` | Healthy. 2 unit tests cover idle budget + runtime smoke. |
| `action/` | `ActionExecutor`, `ActionLog`, `AuditLog` | `d0dc489` | Healthy. 1 unit test covers the executor's reversibility contract. |
| `observer/` | Process scan + classifier | `0a9f321 fix(ayama): detect League of Legends + Lossless Scaling` | Recently extended for new game detection. No new test in that commit — observer is integration-tested through bench. |
| `policy/` | `AutoPolicySelector` + per-game memory | `0a9f321` (transitively) | Healthy. No standalone test file; covered through `bench/`. |
| `bench/` | A/B/A/B/A protocol, PresentMon driver, perceptual metrics, baseline statistics | `b658345 fix(ui): harden non-ASCII text handling` | 3 unit tests. The perceptual-metrics test is the most fragile (statistical); has historically been the one that flakes if randomness is uncontrolled. |
| `ipc/` | SHM publisher between agent & UI | `0dcb04e fix(ayama+ui): prefer discrete GPU` | Healthy. No standalone test — IPC is round-tripped through `tools/ayama-ui` in dev. |
| `config/` | `policies.toml`, `overrides.txt`, `ConfigStore` | `0a9f321` (transitively) | Healthy. No standalone test file. |
| `learn/` | `PerGameMemory` (memory.toml learned outcomes) | `0a9f321` | Healthy. |
| `tools/ayama-{agent,ui,cli,bench}/` | The 4 binaries | `f2cf359 zero-DLL distribution + Defender exclusion` | Zero-DLL distribution landed; static MinGW runtime via `phyriad_static_mingw_runtime()`. |
| `tools/installer/` | `ayama-service-register.exe` | older — see `f2cf359` window | Windows service install path. Not tested in CI. |

**No subdir is in known-broken state** as of the doctor-green snapshot
from 2026-05-23. The unverified gates (G2/G3/G4/G7) need fresh runs
to claim green; they are not known-red.

---

## §4 — Coordination — which session owns what

Per [`docs/training/SESSION_MAID_PROTOCOL.md`](../../training/SESSION_MAID_PROTOCOL.md)
§3 conflict resolution. **Other active sessions DECLARED their mandate
away from `apps/ayama/`**; touching that path from those sessions
would be a §A audit violation.

| Session | Touches Ayama? | Coordination |
|---------|----------------|--------------|
| `cross-session greeting` (this) | YES — `apps/ayama/README.md` only (doc cleanup). NOT framework code, NOT pillar logic. | Owns: G5 doc cleanup, this tracker doc. Does NOT own: G2/G3/G4 (needs build runs the operator decides when to run). |
| `arch P2 — L2b Blackboard` | NO — declared `framework/architecture/**` only. | Out of mandate for Ayama. Read-only welcome; no write coordination needed. |
| `myriarch + meta infra` | NO — declared `apps/myriarch/**` + meta scripts. `apps/ayama/` is explicitly outside. | Out of mandate. Read-only welcome. |
| `operator-maid bridge` | NO — declared `BRIDGE_PROTOCOL.md` + maid log + memory. | Out of mandate. Read-only welcome. |

**What this means for the user.** Closing the unverified gates (G2,
G3, G4, G7) is **operator-decided work**, not LLM-session-claimable
without expanding the scope of one of the active sessions. The natural
moves:

1. The operator opens a new session declared specifically for
   `apps/ayama/**` + `framework/**` + `.github/workflows/**` and runs
   the four unverified gates locally.
2. Or one of the three existing sessions (likely `myriarch + meta
   infra`, who already touches `scripts/*` + CI-adjacent paths)
   expands their declared scope via a `## Mensajes entre sesiones`
   query to include the Ayama build + test runs.
3. Or the operator runs the gates directly (no LLM needed for
   `ctest`).

Any of these closes the loop.

---

## §5 — What's NOT in scope of "stable" (and why)

The following are explicitly **out of scope** for the
`0.1.0-experimental` "stable" milestone, even though they appear on
the README's future-work radar:

- **Code signing** (G5-adjacent). Requires a paid certificate or a
  SignPath open-source enrolment. The README now cites it as a "future
  hardening milestone" with no committed timeline — that is the honest
  framing.
- **Empirical sweep on Intel hybrid + non-X3D AMD.** Requires
  operator-owned hardware the project does not have. The README
  invites PRs; the strategy code is in place.
- **Anti-cheat compatibility.** Out of scope by design (see
  `apps/ayama/README.md` "What Ayama does and does NOT do" + "Always
  read the AntiCheat notice").
- **Public release of the v1.x report archive.** The reports exist; the
  raw CSV data is gitignored (`docs/ayama/reports/raw_data/` per
  `.gitignore` line 49). Operator decides if/when to publish the
  archive.
- **Kernel-mode driver / hardware probe extensions.** Explicitly
  non-goals per the README.

These items are tracked in user-facing docs (README) but not gates for
`0.1.0-experimental` stability.

---

## §6 — Honesty footer

- **No fresh `ctest` was run** as part of opening this tracker. G2 +
  G3 are unverified to a degree that "stable" cannot be claimed
  without the operator (or a sub-session) running them. Per
  [`AGENT_PROTOCOL.md`](../../../AGENT_PROTOCOL.md) §7, "Not validated"
  is the honest status until measured.
- **The `cross-session greeting` session is supervisor-only this
  turn** — no maid was launched, no `maid_log/` artifacts produced.
  See the session's row in
  [`SESSION_LOG.md`](../../../SESSION_LOG.md) for the explicit gap.
- **This tracker does NOT extend the §A mandate** of the audit
  harness. It documents the gates and the per-area state; it does
  not give any session the authority to write into Ayama paths their
  declared mandate excludes.
