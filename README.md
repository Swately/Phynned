# Ayama — runtime optimizer for asymmetric & hybrid CPUs

**A non-invasive runtime optimizer for asymmetric multi-CCD and hybrid CPUs,
with an A/B/A/B/A measurement protocol and a 14-test exploratory catalog on
AMD X3D (provisional — see the evidence status below) — ready out-of-the-box
for Intel hybrid (P+E) and non-X3D multi-CCD AMD.**

> `0.1.0-experimental` · Windows · student-built, LLM-assisted.

Ayama is a **self-contained project**: the Phyriad pillars it consumes
(topology probe, stigmergy `Classifier`, HAL, ETW session, SHM IPC, ImGui
application layer) are vendored under [`framework/`](framework/VENDORED.md)
— its own base, modified at will. It uses the topology probe and the
classifier to pin game threads to the most performant cores available on
your CPU — the V-Cache CCD (AMD X3D), the P-cores (Intel 12th gen+), or a
single CCD (multi-CCD non-X3D AMD) — eliminating cross-CCD / cross-cluster
scheduler thrashing.

---

## Build (from source)

Requires **CMake 3.20+** and a C++23 compiler (built and tested with
MinGW-w64 GCC on Windows). The first configure downloads GLFW 3.4 and
ImGui v1.91.5-docking via FetchContent (network required once).

```
build.bat            ->  build\ayama-dist\             (dev build + tests)
build-release.bat    ->  build-release\ayama-dist\     (LTO, stripped)
testbench.ps1        ->  build + run the full test suite (49 tests)
```

The dist layout puts the entry point on top:

```
ayama-dist/
  ayama-ui.exe                 <- start here
  runtime/                     <- agent, cli, bench, service installer
  scripts/                     <- Defender exclusion helper
```

## Quick start (how to use Ayama)

Ayama ships as a bundle of small executables that live next to each other
in a single directory (the in-tree dev build puts them in
`build/ayama-dist/`; installs place them in `%LOCALAPPDATA%\Ayama`). Choose
whichever entry point matches how you want to use it.

### The easy path — just open the UI

The most common path. Launch `ayama-ui.exe` and everything else happens
for you:

  1. Double-click `ayama-ui.exe`.
  2. Windows asks for elevation (UAC) — say **Yes**. Ayama needs
     administrator rights to set CPU affinity on game processes owned by
     other users and to start an ETW kernel session.
  3. Read the **AntiCheat compatibility notice** that appears on every
     launch. If you understand the trade-off, click **I understand and
     accept the risk** to enter the main window.
  4. The UI auto-spawns `ayama-agent.exe` in the background — you'll see
     `[OK] Agent: live` in the status bar.
  5. Launch a game. Within a few seconds the **Dashboard** tab will list
     it under *Optimized processes / Games (pinned to V-Cache / P-cores)*.
  6. Close the UI window when you're done; the background agent is
     terminated automatically (Windows Job Object teardown).

### The headless path — just run the agent

If you don't need the visual dashboard (e.g. running on a streaming /
recording rig where every UI thread is overhead), launch the agent
directly:

```cmd
ayama-agent.exe
```

The agent attaches itself to a low-priority "self-pin" core, opens the
shared-memory publisher so any future UI / CLI invocation can see its
state, and runs forever. Stop with `Ctrl+C`.

### The CLI path — scripting & A/B benchmarks

`ayama-cli.exe` exposes the same controls the UI uses, plus reporting
helpers for the benchmark protocol:

```cmd
ayama-cli list                       :: show currently observed targets
ayama-cli pin   --pid 1234 --mask 0xFF  :: force-pin a process
ayama-cli revert --pid 1234          :: undo
ayama-cli revert --all               :: undo every active policy
```

The `ayama-bench.exe` companion runs the A/B/A/B/A measurement protocol
end-to-end (PresentMon capture + statistical aggregation + Markdown
report). See `--help` on each tool for full options.

### Service install (set-and-forget on Windows)

For users who want the agent to run as a system service that starts with
Windows:

```cmd
ayama-service-register.exe install --path "C:\Program Files\Ayama\ayama-agent.exe"
ayama-service-register.exe start
```

The service runs as `LocalSystem` so it gets admin rights without UAC
prompts. Remove with `ayama-service-register.exe uninstall`.

---

## Windows Defender — first-launch false positives

Ayama binaries are **unsigned** in `0.1.0-experimental` (no commercial
code-signing certificate). Windows Defender and SmartScreen often flag unsigned
executables that change process affinity + spawn child processes as
"potentially unwanted" — even though Ayama only uses public Win32 APIs
(no kernel driver, no code injection, no game-memory reads).

If Defender blocks `ayama-ui.exe` on first launch, two options:

### Option A — Whitelist Ayama (recommended)

Right-click `scripts/add-defender-exclusion.ps1` → **Run with PowerShell**
(as Administrator). The script:
- Adds the Ayama install directory + `%LOCALAPPDATA%\Ayama` to Defender's
  exclusion paths.
- Adds each Ayama executable (`ayama-ui.exe`, `ayama-agent.exe`,
  `ayama-cli.exe`, `ayama-bench.exe`, `ayama-service-register.exe`) as
  process exclusions.

Reverting is one command:
```powershell
.\scripts\add-defender-exclusion.ps1 -Remove
```

Defender stays fully active for everything else — only Ayama is excluded.

### Option B — Submit to Microsoft as a false positive

If you'd rather not whitelist anything, submit the binaries to
[submit.microsoft.com](https://www.microsoft.com/en-us/wdsi/filesubmission)
as a false positive. Microsoft's analysts typically respond within 48
hours and update SmartScreen reputation accordingly. Once enough users
download Ayama, the reputation builds automatically.

### Why this happens

Process-affinity tools have a long history of being bundled with cheats
or grey-market "optimizers", so Defender's heuristics tend to flag any
binary that calls `SetProcessAffinityMask` on processes owned by other
users. Ayama's design (Win32-only, no kernel driver, no code injection)
is verifiable from the source — see [`action/`](action/) for the
complete list of API calls.

**Long-term fix**: code signing via a commercial certificate (~$200/year)
or the free SignPath open-source program. Tracked as a future hardening
milestone (no committed timeline — `0.1.0-experimental` does not promise
signed builds).

For the full design rationale — why a script instead of disabling Defender,
why we don't sign yet, license compatibility for the static MinGW runtime
link, what the script does *not* do — see
[**`docs/WINDOWS_DISTRIBUTION.md`**](docs/WINDOWS_DISTRIBUTION.md).

## Configuration files

User-editable files live in `%LOCALAPPDATA%\Ayama\` on Windows and
`~/.config/ayama/` on POSIX:

| File             | Purpose                                                 |
|------------------|---------------------------------------------------------|
| `policies.toml`  | Per-rule overrides (enabled/disabled, confidence, mask) |
| `overrides.txt`  | Manual `exe → TargetKind` overrides (UI-editable)       |
| `memory.toml`    | Per-game learned good/bad outcome history (PerGameMemory) |
| `audit.bin`      | Binary action audit log                                 |
| `classifier.toml`| Cached classification decisions (auto-managed)          |

The agent **hot-reloads `overrides.txt`** whenever its mtime changes, so
edits made from the UI's *Targets > Manual classification overrides*
section take effect on the agent's next tick (~100 ms).

## What Ayama does and does NOT do

**Does:**
- Read the Windows process table and your CPU's topology.
- Call `SetProcessAffinityMask` and `SetPriorityClass` on game processes
  it identifies as performance-sensitive.
- Track which optimizations produced positive results per-game and
  remember them across launches (`memory.toml`).
- Auto-revert an optimization if frame times get *worse* after it was
  applied (`AutoRevertGuard` + `PerGameMemory` learning).

**Does NOT:**
- Inject code into games.
- Read game memory.
- Modify game files, shaders, or graphics drivers.
- Send telemetry over the network (no analytics, no phone-home).

This makes Ayama safe with most anticheat systems — but kernel-level
anticheat may still flag the OS-level affinity changes as suspicious.
**Always read the AntiCheat notice in the UI and exclude competitive
games from optimization** if you're risk-averse.

---

## TL;DR

The flagship empirical evidence (X3D) — 14 A/B/A/B/A tests on 7950X3D + RTX 4090.

> **Evidence status (honest):** this dataset is **provisional and under re-validation** — the
> [summary's own banner](docs/reports/EMPIRICAL_EVIDENCE_SUMMARY.md) states its specific
> numbers must **not** be cited as validated (the raw captures were rewritten during a
> processing pass, so the reports cannot be traced to untouched originals; a clean re-run is
> pending). The table below shows what the exploratory runs reported, as a direction — not a
> validated benchmark.

| Game | Engine | Δ Avg FPS | Verdict |
|---|---|---:|---|
| **Halo CE Anniversary** | Halo CE engine (2001) | **+98%** | SIGNIFICANT ⭐ |
| Halo 2 MCC | Halo 2 engine (2004) | +56% | SIGNIFICANT |
| Fallout 4 (modded) | Creation Engine (2015) | +35% | SIGNIFICANT (VSync-capped) |
| Halo 3 MCC | Halo 3 engine (2007) | +19% | SIGNIFICANT |
| Hogwarts Legacy | Unreal Engine 4.27 (2023) | +15.5% | MARGINAL |
| RDR2 (Saint Denis night) | RAGE + Vulkan (2018) | +13.3% | SIGNIFICANT |
| ... | ... | ... | ... |
| Halo Reach MCC | Halo engine mature (2010) | +1.8% | NULL (variance reduction) |
| Halo 4 MCC | Halo engine refresh (2012) | 0% | NULL (engine ceiling) |

See [the full empirical evidence summary](docs/reports/EMPIRICAL_EVIDENCE_SUMMARY.md)
for all 14 reports with raw data and methodology. Intel hybrid and non-X3D
multi-CCD AMD are code-tested but the empirical sweep is planned for a
future release — **PRs welcome** if you can run the A/B/A/B/A protocol on
those.

---

## What makes Ayama different

| | Ayama | Process Lasso | Win 11 Game Mode |
|---|---|---|---|
| Hardware target | Asymmetric / hybrid CPUs (X3D, Intel P/E, non-X3D multi-CCD) | any CPU | any |
| Cost | Free (MIT) | $30 (one-time) | Free (bundled) |
| Empirical evidence published | **14 exploratory reports on X3D** (provisional, re-validation pending) | marketing only | none |
| Auto-revert on regression | **Yes** | No | No |
| Per-game memory | **Yes** | partial | No |
| Open source | **Yes** | No | No |
| Anti-cheat compatible | No (out of scope) | partial | yes |

Ayama is **not** trying to replace Process Lasso. It's a focused optimizer
for asymmetric topologies (V-Cache CCDs, P/E cores, multi-CCD) that publishes
its measurement protocol and per-game reports — something no comparable tool
does — with the dataset's provisional status stated rather than hidden.

---

## Quick start (end users)

### Requirements

- Windows 11 (10 may work, untested)
- One of the supported CPU families below (Ayama auto-detects which strategy to use):

| CPU family | Strategy auto-selected | Empirical validation |
|---|---|---|
| **AMD X3D dual-CCD** (7900X3D, 7950X3D, 9950X3D) | Pin games to V-Cache CCD; evict background to non-V-Cache CCD | ⭐ **14 exploratory reports** (provisional) |
| **AMD X3D single-CCD** (7800X3D, 9800X3D) | Pin games to V-Cache cores; no eviction needed | Strategy proven on dual-CCD; single-CCD inherits the pinning half |
| **Intel hybrid** (12th gen+ Alder/Raptor/Meteor/Arrow Lake) | Pin games to P-cores; move background to E-cores | ✅ Code-tested; empirical sweep deferred to a future release |
| **AMD multi-CCD non-X3D** (5950X, 7950X, 9950X) | Isolate games on CCD0; background on CCD1+ | ✅ Code-tested; reduces cross-CCD migrations |
| **Symmetric single-CCD** (any) | Monitor only (no benefit from affinity) | N/A — Ayama skips affinity changes |

- Administrator privileges (required for ETW and cross-process affinity)

### Install

1. Download the latest `ayama-x.y.z-windows-x64.zip` release (link
   published once the project's public repository is up; until then,
   build from source — see **Build** above)
2. Extract to any folder
3. Right-click `install.ps1` → Run with PowerShell (as Administrator)
4. Launch `Ayama` from Start menu

### First run

1. Start any game (single-player, no anti-cheat — see [FAQ](docs/FAQ.md))
2. Open Ayama UI
3. Tab "Dashboard" shows real-time agent status
4. Tab "Benchmark" runs A/B/A/B/A protocol to measure the impact

---

## How Ayama uses the vendored framework

| Vendored pillar | Ayama use |
|---|---|
| `phyriad::hal` / `phyriad::topology` | CPU topology (CCD enumeration, V-Cache detection, P/E core classification), affinity APIs |
| `phyriad::process` | Bulk process enumeration via `NtQuerySystemInformation` |
| `phyriad::stigmergy::Classifier` | `ProcessClassifier` (process → tier), `AutoPolicySelector` (signal → action) |
| `phyriad::transport::Ring` | `ActionLog` circular ring for the per-tick action history |
| `phyriad::ipc` | Shared-memory channel between `ayama-agent` and `ayama-ui` |
| `phyriad::ui` (ImGui) | Dashboard + benchmark runner |
| `phyriad::etw` | Read-only kernel session for game detection |

The full pillar list and why each is vendored:
[`framework/VENDORED.md`](framework/VENDORED.md).

Note: the Ayama agent is single-threaded by design (one tick every 100 ms),
so it does **not** use a thread pool for work distribution. The decision
loop is per-target, not per-task.

The pattern this codifies — observe shared state, decide, emit
trace — is the stigmergic-dispatch pattern documented in
[docs/STIGMERGY.md](docs/STIGMERGY.md). Ayama was the original
production use case; the pattern was extracted afterward into
`phyriad::stigmergy::*` as a library pillar.

---

## Documentation

### For users
- **[FAQ](docs/FAQ.md)** — hardware support, anti-cheat policy, alternatives
- **Reproducing benchmarks** (coming Tier 1) — replicate any test in the catalog

### For developers
- **[ARCHITECTURE.md](docs/ARCHITECTURE.md)** — module map + vendored pillar graph
- **[framework/VENDORED.md](framework/VENDORED.md)** — the vendored substrate snapshot and its provenance
- **Source layout** — `core/` (AgentRuntime), `observer/` (process scanning + classifier),
  `policy/` (rule engine), `action/` (effect executor), `learn/` (per-game memory),
  `bench/` (A/B/A/B/A protocol), `ipc/` (UI ↔ agent SHM), `config/` (TOML stores),
  `tools/` (agent, ui, cli, bench, installer)

### For researchers
- **[Empirical Evidence Summary](docs/reports/EMPIRICAL_EVIDENCE_SUMMARY.md)** — all 14 tests + predictive model
- **[Empirical Test Protocol](docs/EMPIRICAL_TEST_PROTOCOL.md)** — methodology
- **[Per-game reports](docs/reports/)** — raw data, per-run analysis

---

## License

Released under the [**MIT License**](LICENSE) — free to use, modify, and
distribute. © 2026 Eduardo Ramos Mendoza (Swately).

If you use Ayama or any of its code, the MIT license requires you to
**keep the copyright notice**, i.e. to **credit Eduardo Ramos Mendoza
(Swately)**. A visible mention in your project's credits, documentation,
or about screen is appreciated. Ayama is a research/engineering artifact —
provided "as is" with no warranty.

## Citation

```bibtex
@software{ayama_2026,
  author = {Ramos Mendoza, Eduardo},
  title  = {Ayama: A Non-Invasive Runtime Optimizer for Asymmetric and
            Hybrid CPUs},
  year   = {2026},
  note   = {14-test exploratory A/B catalog (provisional, re-validation
            pending) with predictive model}
}
```
