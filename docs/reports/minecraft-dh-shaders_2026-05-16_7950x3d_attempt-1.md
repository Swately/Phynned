# Scenario: Minecraft Java + Distant Horizons + Complementary Shaders

**Status: INVALID — policy not applied. Documented for honesty (Master Plan §0.5 #6).**

## Hardware

- CPU: AMD Ryzen 9 7950X3D (16 cores, dual CCD asymmetric V-Cache)
- RAM: 32 GB DDR5 — Minecraft asignados 8 GB heap
- GPU: NVIDIA RTX 4090
- OS: Windows 11

## Software

- Ayama: build commit pre-fix-#11 (mayo 16 2026)
- Minecraft: Java Edition 26.1.2
- Sodium: 0.8.9
- Iris: 1.10.9
- Distant Horizons: 2.x — Quality Medium, CPU Load Balanced, **16 threads**, LOD radius **512 chunks**, todos los chunks pre-cargados
- Shader: Complementary Reimagined Unbound v5.7.1, **Preset High**
- PresentMon: 2.4.1

## Workload

- Seed: `-5404931980431803324`
- Spawn position: `-650, 175, -348`
- Test scene: creative + speed-60 + vuelo dirección este (+X) por 90 s
- Captura: PresentMon 2.x con `--timed 90 --terminate_after_timed`

## Phase A — Baseline (Ayama OFF)

```
-- PresentMon import summary -------------------------
  Total frames: 18479
  Avg FPS:      207.5  (FT avg=4.82 ms)
  1% low FPS:   139.2  (FT P99=7.19 ms)
  0.1% low FPS: 27.9   (FT P99.9=35.80 ms)
  Median FT:    4.61 ms (P50)
  Max FT:       66.89 ms
  Std Dev:      1.80 ms
  Stutters:     61  (frames > 2×avg = 9.64 ms)
------------------------------------------------------
```

## Phase B — "Ayama ON" (intended PinGameToVCacheCcd, **NOT applied — see Critical Finding**)

```
-- PresentMon import summary -------------------------
  Total frames: 19933
  Avg FPS:      223.6  (FT avg=4.47 ms)
  1% low FPS:   134.3  (FT P99=7.45 ms)
  0.1% low FPS: 24.1   (FT P99.9=41.41 ms)
  Median FT:    4.24 ms (P50)
  Max FT:       77.98 ms
  Std Dev:      2.18 ms
  Stutters:     163  (frames > 2×avg = 8.94 ms)
------------------------------------------------------
```

## Delta

```
-- ayama-bench diff ------------------------------------------
  Metric                  Baseline     Treated       Delta
  --------------------  ----------  ----------  ----------
  Avg FT (ms)                 4.82        4.47       +7.2%
  P99 FT (ms)                 7.19        7.45       -3.6%
  P99.9 FT (ms)              35.80       41.41      -15.7%
  Variance (ms)               1.80        2.18      -20.9%
  Stutters                      61         163     -167.2%

  Verdict: REGRESSION
```

## Critical Finding — invalid run

**`%LOCALAPPDATA%\Ayama\audit.bin` size = 0 bytes after the test.**
**`memory.toml` does not contain a learned entry for `javaw.exe`.**

→ **Ayama never executed `apply PinAffinity` during Phase B.** The policy was
never applied. What we measured is:

- Phase A: Minecraft solo (clean)
- Phase B: Minecraft + agent running **with overhead but WITHOUT policy active**

The "regression" verdict reflects **agent overhead, not policy effect**.

### Why the policy never applied

Root cause: **Bug #11** in `ForegroundWatcher::on_tick()` — the
`foreground_for_ms_` counter resets to 0 on every HWND change. Every alt-tab
the user does to verify via `ayama-cli` (e.g. checking `targets`/`actions`)
breaks the 30-second classification threshold. Net effect: `javaw.exe` never
crossed `foreground_for_sec > 30u` → never reached `kind=Game` → no rule
fired → no `apply PinAffinity` → empty audit log.

Fix already implemented (commit post-2026-05-16): cumulative per-PID
foreground tracking that survives alt-tabs. Test must be re-run with new
binary.

### Quantification of agent overhead (incidental signal)

Phase B captured 19,933 frames vs 18,479 baseline (8% more). Avg FT actually
*improved* +7.2% — likely because the agent's CSwitch ETW callback at ~5-14%
CPU on **non-V-Cache CCD (CCD1, where the agent self-pins)** had no contention
with Minecraft (running freely across all 32 logical cores). The improved avg
is consistent with cache warm-up effects from the longer capture window.

The P99.9 degradation (-15.7%) and stutter triplication (61 → 163) are
attributable to:
- ETW kernel CSwitch events firing at very high rate
- Agent self-monitor reports CPU=5-14% (vs target 0.3-1%)
- Background CPU contention indirectly affecting LWJGL renderer thread scheduling

This is a useful negative data point: **the agent's current ETW overhead is
detectable in tail latencies even when policy is not applied.** Tracked as
follow-up Master Plan §11.5.

## Verdict

**INVALID — re-run required.**

Bugs caught during this attempt (10 cumulative):

| # | Bug | Status |
|---|-----|--------|
| 1 | InternalWatchdog non-interruptible sleep | Fixed |
| 2 | PrivilegeCheck Win32 used GetPriorityClass | Fixed |
| 3 | check_d3d_vk_modules missed OpenGL/LWJGL | Fixed |
| 4 | AgentRuntime never called add_target_pattern | Fixed |
| 5 | update_target had no per-name dedup → 32-slot exhaustion | Fixed |
| 6 | Topology detector reports single-CCD on 7950X3D | Open (no impact on mask) |
| 7 | ClassificationCache TTL re-classify with stale signals | Fixed (#10) |
| 8 | classify threshold 3/4 too strict for graphics apps | Fixed (#9) |
| 9 | Visual encoding rotated UTF-8 → cp1252 in PowerShell | Fixed |
| 10 | ClassificationCache could downgrade Game → Unknown | Fixed |
| 11 | ForegroundWatcher reset counter per HWND change | Fixed (this commit) |

## Next steps

1. Stop running agent (release `ayama-agent.exe` for rebuild).
2. Rebuild with fix #11.
3. Re-run Phase B procedure — but **DO NOT alt-tab during the 30-second warm-up**.
4. Verify `actions` shows `apply PinAffinity` BEFORE PresentMon capture.
5. Verify `audit.bin` > 0 bytes after the run.

## Observations to follow up

- **Agent overhead (5-14% CPU)** needs reduction before benchmarks are valid.
  Likely needs ETW provider filtering or kernel keyword filter to reduce
  CSwitch event rate. Tracked Master Plan §11.5.
- **Distant Horizons workload may not benefit from V-Cache pinning** even when
  applied correctly: DH spawns 16 worker threads, V-Cache CCD has 8 phys
  (16 SMT) cores at lower clocks. Pinning to V-Cache may be net-neutral or
  negative for heavily threaded scenarios. Test will tell.
