# Phynned — architecture

> Self-contained since the 2026-07-16 separation (see
> [`plans/AYAMA_SEPARATION_MASTER_PLAN.md`](plans/AYAMA_SEPARATION_MASTER_PLAN.md)).
> The vendored substrate snapshot is documented in
> [`../framework/VENDORED.md`](../framework/VENDORED.md).

## The system in one paragraph

Phynned is three cooperating processes. **`phynned-agent`** (the brain, admin) runs a
single-threaded 100 ms tick: an ETW kernel session + process-table scan feed a
classifier that decides which processes are performance-sensitive games; a rule
engine turns that into affinity/priority actions; an executor applies them with
only three Win32 calls (`SetProcessAffinityMask`, `SetPriorityClass`,
`SetThreadAffinityMask`) and logs every action; an auto-revert guard undoes
anything that made frame times worse, and everything reverts on exit — crash
included. **`phynned-ui`** (the face) is an ImGui window that reads the agent's
state over shared memory and never touches a game itself. **`phynned-cli`** and
**`phynned-bench`** script the same controls and run the A/B/A/B/A measurement
protocol.

## Module map (each module = one static lib + its tests)

| Module | Responsibility | Key types |
|---|---|---|
| `observer/` | See the machine: ETW session, process/thread enumeration, per-process metrics, classification | `ProcessObserver`, `ProcessClassifier`, `MetricsCollector`, `EtwProviderSet` |
| `policy/` | Decide: rules + conditions → decisions; auto-selection of the per-CPU strategy | `PolicyEngine`, `Rule`, `Condition`, `AutoPolicySelector` |
| `action/` | Act: apply affinity/priority, audit every action | `ActionExecutor`, `ActionLog`, `AuditLog` |
| `learn/` | Remember: per-game outcome history, stale-entry expiry | `PerGameMemory`, `LearnedEntry` |
| `bench/` | Prove: A/B/A/B/A runner, PresentMon capture, statistics, perceptual metrics | `ABRunner`, `Baseline`, `DiffReport`, `PerceptualMetrics` |
| `ipc/` | Publish: agent → UI shared-memory channel | `PhynnedAgentPublisher`, `PhynnedClient`, `PhynnedProtocol` |
| `config/` | Configure: TOML stores, default policy pack | `ConfigStore`, `DefaultPolicyPack` |
| `core/` | Orchestrate: the tick loop, watchdogs, self-monitoring, single-instance | `AgentRuntime`, `AdaptiveTick`, `AutoRevertGuard`, `InternalWatchdog`, `SelfMonitor` |

Build order (leaf-first, from the root `CMakeLists.txt`):
`observer → learn → policy → action → bench → ipc → config → core`.
`core` privately depends on all others.

## Process topology

```
phynned-ui.exe (user, UAC-elevated)
   │ spawns + Job-Object-owns
   ▼
phynned-agent.exe ──────────── ETW kernel session (read-only)
   │  SHM publisher                │
   ▼                               ▼
phynned-ui / phynned-cli read     process table scan
state over phyriad::ipc       (NtQuerySystemInformation)
```

`phynned-service-register.exe` optionally installs the agent as a
`LocalSystem` service instead of the UI-spawned model.

## The vendored framework (17 units)

Phynned's substrate is its own snapshot under `framework/` — the project builds
with zero references outside its tree. Direct consumers: `hal`, `schema`,
`topology`, `tuning`, `process`, `stigmergy`, `etw`, `ipc`, `node`, `ui`, plus
`<phyriad/version.hpp>` from `_meta`. Transitives: `graph` (hosts the
`phyriad/api/*` headers), `scheduler`, `transport`, `render` (ui backend),
`runtime` + `profile` (hard-linked by `phyriad_ui`). Trimmed as unused:
`orchestration`, `behavior`, `correlation`. Full rationale per unit:
[`../framework/VENDORED.md`](../framework/VENDORED.md).

GLFW 3.4 and ImGui v1.91.5-docking are fetched at configure time (the same
pinned tags the substrate used); they are the only network dependency.

## Testing

`testbench.ps1` configures with `-DPHYNNED_BUILD_TESTS=ON -DPHYRIAD_BUILD_TESTS=ON`,
builds, **asserts the discovered-test set is non-empty** (a ctest quirk returns
exit 0 on an empty set — that false green hid a failing test for ~9 weeks until
2026-07-16), and runs everything: Phynned's 10 module tests + the vendored
pillars' own suites (49 total at separation). Known coverage gaps, recorded
honestly: `config/` has no test source (`phynned_config_test` is declared behind
an `if(EXISTS)` that has never been true) and `ipc/` has no tests at all (no
`tests/` directory, no `add_test`) — 6 of the 8 modules are covered.

Known failing test under elevation (found 2026-07-16, pre-dates the rename):
`agent_idle_budget_test` asserts `stop()` completes in < 2 s, but when the
suite runs **as Administrator** the ETW session actually starts and its
teardown takes ~5.1 s (measured twice: 5069/5085 ms) — the bound was only
ever exercised unelevated, where ETW never engages. Open defect: either the
shutdown path must interrupt the ETW consumer faster, or the bound must
encode the elevated reality. 48/49 pass elevated; 49/49 unelevated.

<!-- Made with my soul - Swately <3 -->
