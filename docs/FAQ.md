# Ayama / Phyriad FAQ

## General

### What is Ayama?

A runtime CPU optimizer for AMD asymmetric V-Cache CPUs (7950X3D, 7900X3D,
9950X3D class). It detects games via foreground heuristics, classifies them,
and pins them to the 3D V-Cache CCD to reduce cross-CCD scheduler thrashing.

### What is Phyriad?

A personal, LLM-assisted C++23 knowledge-and-code substrate — the C++23
systems-programming foundation underneath Ayama. It provides:

- Cross-platform hardware topology probing (CPU/cache/CCD layout)
- Bulk process metrics in one syscall
- Lock-free shared-memory IPC with POD-only contracts
- ETW session management abstracted to ~50 lines
- Privilege detection and graceful degradation

Phyriad can be used standalone (see the project's main [README](README.md) for QUICKSTART links).

### Is Ayama free?

Yes — **MIT License** (re-licensed from PolyForm Noncommercial at the
2026-07-16 separation, matching PhyriadFG). Free to use, modify, and
distribute, commercially included; the only condition is keeping the
copyright notice (credit Eduardo Ramos Mendoza / Swately). No telemetry.
No license server. No phone-home.

### Does Ayama replace Process Lasso?

No. Process Lasso has 20+ years of feature surface (ProBalance, GPU PerformanceMode,
power profile automation, etc.) that Ayama deliberately does not duplicate.
Ayama is focused on **one thing**: V-Cache CCD affinity management with
empirical validation. It complements Process Lasso rather than competes.

If you need broad process-control features, use Process Lasso. If you specifically
want an open, evidence-backed V-Cache optimizer for AMD X3D, use Ayama.

---

## Hardware support

### What CPUs does Ayama support?

| CPU class | Ayama action | Expected benefit |
|---|---|---|
| AMD 7950X3D / 7900X3D / 9950X3D (dual-CCD V-Cache) | **Primary target** | Up to +98% FPS in extreme cases (see catalog) |
| AMD 7800X3D / 9800X3D (single-CCD V-Cache) | Detects, but no V-Cache eviction policy applies (all cores have V-Cache) | Variance reduction only |
| Intel hybrid (12th-14th gen, Core Ultra) | Has policies (P-core pinning, E-core eviction) but **not empirically validated** | Untested |
| AMD non-X3D (5950X, 7900, 7950, etc.) | Detects symmetric topology, applies background eviction only | Marginal |
| Older / non-asymmetric | No action | None |

### Will Ayama work on my GPU?

GPU brand and model don't matter for Ayama's core function. Ayama is a CPU
optimizer — the GPU just needs to be powerful enough that you're CPU-bound in
the workload you care about. If GPU sits at 95%+ during gameplay, lower
graphics settings until GPU drops to 60-85% — only then Ayama has room to help.

### Does it work on Linux?

Most Phyriad pillars have Linux paths but they're **stubs or incomplete**. Ayama
itself is Windows-only at present. Linux support is roadmap, not promised.

---

## Anti-cheat compatibility

### Can I use Ayama with anti-cheat games?

**Strongly recommended: no.**

Ayama performs operations that *may* be flagged by kernel-mode anti-cheats:

- Opening other processes with `PROCESS_QUERY_LIMITED_INFORMATION` and `PROCESS_SET_INFORMATION`
- Modifying CPU affinity of other processes
- Running an ETW session that captures context-switch events

Anti-cheats explicitly flagged as risky (use of Ayama not recommended):

- **Riot Vanguard** (Valorant, League of Legends client)
- **EasyAntiCheat (EAC)** (Fortnite, Apex Legends, Elden Ring, etc.)
- **BattlEye** (PUBG, Rainbow Six Siege, DayZ, etc.)
- **Ricochet** (Call of Duty Modern Warfare/Warzone)
- **EA Javelin** (Battlefield 6, Madden, FC, etc.)

You will not get banned for *running Ayama with the agent off*. The risk is
when the agent is actively pinning the game process. If you must run Ayama
on a system with an anti-cheat game installed:

- Stop the agent before launching the anti-cheat game
- Or only run the agent on a separate user account / VM

### What games are safe to test with Ayama?

- Single-player only games without kernel-mode anti-cheat (most AAA single-player titles)
- Modded games (Minecraft, Fallout 4 / Skyrim, Witcher 3, etc.)
- Older games (anything pre-2010 typically has no anti-cheat at all)
- Anything in the empirical catalog in [reports/](reports/) is safe

When in doubt, check the game's anti-cheat backend before enabling Ayama.

---

## Empirical evidence

### Why publish so much benchmark data?

Because nobody else does, and that's a real differentiator. Process Lasso,
Windows Game Mode, AMD's V-Cache optimizer driver — all of them make
performance claims without publishing statistically rigorous data. Ayama
publishes the methodology, raw CSVs, per-run data, aggregate stats, and
predictive model so anyone can reproduce or refute.

### Why does my benchmark not match yours?

Common reasons:

1. **You're GPU-bound** (see Tip above). Lower graphics until GPU < 85%.
2. **Different scene.** Saint Denis (RDR2 city night) is CPU-heavier than free-roam horizon.
3. **Different engine variant.** Same game can ship with different threading on different platforms.
4. **VSync / framerate cap** masks Ayama's improvement.
5. **Anti-cheat process loaded** even if game isn't actively playing — denies Ayama access.

If you've controlled for all of these and still see mismatch, open an issue
with your methodology and CSV data attached.

### What if Ayama hurts performance in my game?

Ayama has an **auto-revert guard** that detects frame-time variance regressions
and automatically reverts policies for the offending process. If you see
auto-revert kicking in, the game gets added to a per-game "bad list" stored in
`%LOCALAPPDATA%\Ayama\memory.toml` and won't be touched again for 30 days.

If auto-revert doesn't catch it, you can manually pause Ayama via the UI Dashboard
or by sending a Pause IPC command.

---

## Performance

### How much CPU does Ayama use?

Budget: `< 3%` sustained on a 7950X3D during active operation, `< 0.5%` when idle.
The agent self-monitors and reports `Budget exceeded` to stderr if it crosses
the limit. In our long-running tests, agent's own CPU footprint stays in the
1.5-2.9% range under typical gaming load.

### How much RAM?

Budget: `< 50 MB` RSS active, `< 20 MB` RSS idle. The agent uses
`SetProcessWorkingSetSizeEx` to hint to Windows the working-set target.

### Will it slow down my games as background noise?

Designed to not. The agent self-pins to a "low value" core (non-V-Cache CCD)
so its own activity doesn't contend with the game it's optimizing. Plus it
uses adaptive tick intervals (slower when no game is running).

---

## Configuration

### Where are config files stored?

`%LOCALAPPDATA%\Ayama\`

- `policies.toml` — rule overrides (priority, enable/disable per rule)
- `memory.toml` — per-game memory (which policies worked for which games)
- `audit.bin` — append-only log of every policy applied / reverted (for debugging)

These are auto-generated on first run with sensible defaults. Edit only if you
know what you're doing.

### How do I disable Ayama for a specific game?

Two options:

1. **UI**: open the "Targets" panel, find the game, click "Disable Ayama for this exe"
2. **Manually**: edit `policies.toml` and add the game's executable name to the bad list

### How do I run a custom benchmark?

UI tab "Benchmark":

1. Pick the game from active targets (or type its exe name manually)
2. Set duration (30 s default, 90 s for serious tests)
3. Click "Run A/B/A/B/A protocol (5 runs)"
4. Wait ~3 min
5. Read aggregate verdict in the panel

The full methodology is in [EMPIRICAL_TEST_PROTOCOL.md](EMPIRICAL_TEST_PROTOCOL.md).

---

## Troubleshooting

### Agent crashes on startup

Most likely:
- **Not admin**: relaunch as Administrator
- **Already running**: check `Get-Process ayama-agent` and stop the existing instance
- **Locked binary**: Windows Defender is scanning; wait 30 s and retry

### UI doesn't show the agent's data

The UI reads shared memory from the agent. Check:
- Agent is running (`Get-Process ayama-agent`)
- Agent has admin (UI works without admin, but agent needs it for SHM creation)
- Both processes are the same user / session

### Bench fails with "PresentMon exited with code 6"

A previous PresentMon ETW session is lingering in the kernel. The bench runner
should auto-recover via `--stop_existing_session`. If not, manually:

```powershell
# From admin PowerShell:
logman stop "PresentMon" -ets
```

### Where do I report bugs?

GitHub issues. Include:

- Hardware (CPU model)
- Windows version (winver)
- Ayama version (`ayama-cli --version`)
- Steps to reproduce
- Relevant log output (agent stdout/stderr)
