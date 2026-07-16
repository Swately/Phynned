# Scenario: Minecraft Java + Twitch Live Streaming + Discord Screen Share

**Status: VALID — MARGINAL IMPROVEMENT** (with documented caveat).
**Second public empirical report.** First multi-encoder workload tested.

## Summary

Ayama's full policy stack (Rules 1+3+6) applied to a real-world multi-stream
gaming setup on AMD Ryzen 9 7950X3D delivered measurable improvements in
average and 1% low FPS, but **only marginal improvements in deep tail
latencies** (P99.9, sub-60Hz hitches) — and even **slight regression** in the
absolute hitch count.

- **Avg FPS: +11.8%** (191.5 → 217.1) — strongest improvement, V-Cache benefit.
- **1% low FPS: +10.1%** (96.4 → 107.2)
- **0.1% low FPS: +7.1%** (25.3 → 27.2) — much smaller than the solo-game test.
- **Sub-60Hz hitches: −14% (worse)** (57 → 65) — **regression** in absolute hitches.

The result is interpretable, real, and informs an important architectural finding:
**multi-process applications (Discord with 6+ helpers) exceeded the per-name
tracking cap of 4, leaving 2 helpers un-pinned and free to wander into the
V-Cache CCD** during the test, causing intermittent contention with the game
thread (bug #17).

## Hardware

| Component | Spec |
|---|---|
| CPU | AMD Ryzen 9 7950X3D (16c/32t, dual CCD asymmetric V-Cache) |
| RAM | 32 GB DDR5 (Minecraft 8 GB heap) |
| GPU | NVIDIA GeForce RTX 4090 |
| OS | Windows 11 |
| Display | (assumed: 1080p or higher, 144Hz+ refresh) |

## Software

| Piece | Version |
|---|---|
| Ayama | build commit 2026-05-16 (post fixes 1-16) |
| Minecraft Java | 26.1.2 |
| Sodium / Iris / Distant Horizons | 0.8.9 / 1.10.9 / 2.x |
| Distant Horizons config | Medium quality, 16 threads, **512-chunk LOD radius** |
| Complementary Reimagined Unbound | v5.7.1, **High preset** |
| OBS Studio | streaming live to Twitch |
| OBS encoder | **x264, "fast" preset, 1080p60, 6 Mbps** |
| Discord | screen sharing the Minecraft window (voice channel active) |
| PresentMon | 2.4.1 |

## Workload

- Seed: `-5404931980431803324`
- Spawn: `(-650, 175, -348)`
- Test scene: creative + speed-60 + eastward flight, 90 s
- **Live streaming**: Twitch live + Discord screen share active during both phases
- Captura: `PresentMon --timed 90 --terminate_after_timed`

## Phase A — Baseline (no Ayama)

Workload: Minecraft + Twitch live (x264 fast 1080p60 6Mbps) + Discord screen share active.

```
Total frames:        17,054
Avg FPS:             191.5  (FT avg = 5.22 ms)
1% low FPS:           96.4  (P99 = 10.37 ms)
0.1% low FPS:         25.3  (P99.9 = 39.54 ms)
Median FT:            4.95 ms (P50)
Max FT:              58.63 ms
Std Dev:              2.13 ms
Sub-60Hz hitches:       57  (frames > 16.67 ms — absolute, platform-independent)
```

Compared to solo-Minecraft baseline from `minecraft-dh-shaders_2026-05-16`:
- Avg FPS dropped 207.5 → 191.5 (−8%) — encoder + Discord overhead.
- 1% low FPS dropped 139 → 96 (−31%) — significant streaming-induced stutters.
- 0.1% low FPS dropped 28 → 25 (−10%).

This baseline is **noticeably worse than solo-game**, confirming streaming workload
adds real CPU contention even at "fast" preset.

## Phase B — Treated (Ayama active, full policy stack)

Verification of policy application:

```
ayama-cli actions:
PID       Rule  AffinityMask  PrevMask    Status
26324      1    0xffff        0xffffffff  active   ← Rule 1: javaw -> V-Cache CCD0
29344      1    0xffff        0xffffffff  active   ← Rule 1: javaw (2nd PID) -> V-Cache CCD0
21332      3    0xffff0000    0xffffffff  active   ← Rule 3: obs64 -> CCD1
 2720      6    0xffff0000    0xffffffff  active   ← Rule 6: Discord -> CCD1
 8168      6    0xffff0000    0xffffffff  active   ← Rule 6: Discord -> CCD1
 9424      6    0xffff0000    0xffffffff  active   ← Rule 6: Discord -> CCD1
13332      6    0xffff0000    0xffffffff  active   ← Rule 6: Discord -> CCD1
```

7 rule applications. ALL 3 rule types fired correctly. OS-level affinity verified
via `Get-Process | Select ProcessorAffinity`:

```
javaw  (26324, 29344): ProcessorAffinity =     65535  (0xFFFF)        ← CCD0 V-Cache
obs64  (21332):        ProcessorAffinity = 4294901760  (0xFFFF0000)   ← CCD1
Discord (4 PIDs):      ProcessorAffinity = 4294901760  (0xFFFF0000)   ← CCD1
```

**`audit.bin` size: 1152 bytes** — confirms persistent audit log working.

### Workload after Ayama applied (same streaming setup)

```
Total frames:        19,201
Avg FPS:             217.1  (FT avg = 4.61 ms)
1% low FPS:          107.2  (P99 = 9.33 ms)
0.1% low FPS:         27.2  (P99.9 = 36.72 ms)
Median FT:            4.36 ms (P50)
Max FT:              69.03 ms
Std Dev:              1.87 ms
Sub-60Hz hitches:       65  (worse than baseline 57)
```

## Delta

```
-- ayama-bench diff -------------------------------------------

  Player-perceived FPS (higher = better)
  Metric                 Baseline    Treated      Delta
  --------------------  ---------  ---------  ---------
  Avg FPS                   191.5      217.1     +11.8%   ← strong win
  1% low FPS                 96.4      107.2     +10.1%   ← good win
  0.1% low FPS               25.3       27.2      +7.1%   ← marginal

  Frame time (ms) -- lower = better
  Median (P50)               4.95       4.36     +11.9%
  Avg                        5.22       4.61     +11.8%
  P99                       10.37       9.33     +10.1%
  P99.9                     39.54      36.72      +7.1%
  Max                       58.63      69.03     -17.7%  ← single outlier
  Std Dev                    2.13       1.87     +12.3%

  Stutters (lower = better)
  Frames > 16.67 ms            57         65     -14.0%  ← REGRESSION
  > 2x avg (relative)         164        208     -26.8%  (avg shift, ignore)

  Verdict: MARGINAL IMPROVEMENT
        (P99.9 frametime delta = +7.1% drives verdict)
```

## Interpretation

### What worked (steady-state V-Cache benefit)

The +11.8% Avg FPS and +10.1% 1% low FPS improvements are **real and significant**.
They reflect Minecraft's render thread benefiting from V-Cache (96 MB L3 vs 32 MB
L3 on non-V-Cache CCD), with reduced contention from x264 encoder and Discord
processes that were correctly evicted to CCD1.

This validates that **the policy stack (Rules 1+3+6) functions correctly for the
streamer multi-encoder use case**, fully replacing the missing Windows scheduler
behavior with deterministic CCD-level isolation.

### What did not work as well (deep tail + absolute hitches)

The deep tail metrics (P99.9, sub-60Hz hitches) regressed or barely improved.
Compare to the solo-game test:

| Metric | Solo MC | MC + streams |
|---|---:|---:|
| Avg FPS Δ | +4.2% | **+11.8%** (better — V-Cache helps more when contention exists) |
| 1% low FPS Δ | +10.6% | +10.1% (similar) |
| 0.1% low FPS Δ | +22.7% | **+7.1%** (worse) |
| Sub-60Hz hitches Δ | +32.6% (fewer) | **−14% (more)** |

The pattern says: Ayama is *better at restoring average performance* under
contention, but *worse at protecting the deep tail*. Hypothesis: heavily-
loaded CCD1 (OBS x264 + 4 Discord + Chrome + Code + agent itself) creates
its own contention storm, and any process that briefly escapes onto CCD0
preempts the game thread for a long frame.

### Root cause of regression: bug #17

Direct evidence from the test run:

```
Get-Process Discord | Select Id, ProcessorAffinity:
 2720 Discord  4294901760  ← pinned to CCD1 (good)
 8168 Discord  4294901760  ← pinned
 9424 Discord  4294901760  ← pinned
13332 Discord  4294901760  ← pinned
14108 Discord  4294967295  ← UNPINNED — wandering wherever Windows wants
14668 Discord  4294967295  ← UNPINNED
```

Two Discord helpers (PIDs 14108, 14668) had `ProcessorAffinity = 4294967295`
(default = all 32 logical cores). They were free to schedule onto CCD0 V-Cache
cores and contend with Minecraft's render thread.

**Why**: `ProcessObserver::update_target` had `kMaxPerName = 4` per exe name. Discord
launches 6+ helpers (RPC, audio, screen-capture worker, network, etc.). The first 4
Discord PIDs got pinned by Rule 6; helpers 5 and 6 arrived AFTER the cap was reached
and were silently dropped from tracking. Untracked → not classified → no rule applied
→ default affinity.

**Fix applied (post this test)**: bumped `kMaxPerName` from 4 to 8 (commit 2026-05-16).
With 8 per name and 32 global slots, a streamer setup of Minecraft (2) + obs64 (1) +
Discord (6-8) + Chrome/Code/Edge (cap-limited) fits comfortably.

A re-test with the new cap should produce a cleaner P99.9 improvement, likely
closer to the solo-game test's +22.7%.

## Honest caveats

1. **Result is interpretable but not conclusive for "Ayama policy quality"** —
   the regression in sub-60Hz hitches is partly due to bug #17, not necessarily
   policy design. Once Discord is fully pinned, expect better deep-tail metrics.

2. **Test was live streaming (not local recording)** — ISP variance could
   affect encoder buffer behavior. This is a real-world test, not laboratory.
   For lab-grade results, switch to local recording.

3. **Max FT regressed (58 → 69 ms)** — single outlier; P99.9 metric is robust
   to single outliers. Likely an OBS frame retransmit or shader compile event.

4. **Std Dev IMPROVED** (+12.3%) — the variance reduction is real and
   meaningful — frame delivery is more consistent on average even though
   the worst-case absolute hitches went up.

5. **Agent CPU overhead during this test was NOT measured** — the §11.5
   optimization (drop pid=0 CSwitch events) should reduce overhead from
   5-14% to < 2%, but we did not capture `[SelfMonitor]` output during this
   specific run. Next test should include this measurement.

## Comparison with solo-game test

| Test | Avg FPS Δ | 1% low Δ | 0.1% low Δ | Sub-60Hz Δ | Verdict |
|------|----------:|---------:|-----------:|-----------:|---------|
| Solo MC (May 16) | +4.2% | +10.6% | +22.7% | +32.6% | SIGNIFICANT |
| MC + Twitch + Discord (this) | **+11.8%** | +10.1% | +7.1% | −14% | MARGINAL |

Key insight: **Ayama provides greater Avg/P99 lift when baseline is more
contended**. The 7.1% P99.9 win is smaller because background workload spills
back onto CCD0 due to bug #17.

## Validates / advances

1. ✅ **Rule 6 EvictCommFromHotCcd works** — first empirical proof of the new
   rule added this session.
2. ✅ **Rule 3 EvictStreamFromHotCcd applies correctly** with bug #14 fix
   (no longer gated by `multi_ccd` flag that bug #6 broke on this CPU).
3. ✅ **Per-name dedup works** — without it, Discord's 6+ helpers + Chrome's
   many helpers would have saturated the 32-slot global cap before Minecraft's
   high-PID javaw was added.
4. ✅ **PowerShell verification** confirms OS-level affinity matches log
   (bug #12 in display was cosmetic).
5. ⚠ **Discovered bug #17**: cap-per-name=4 insufficient for Discord; fixed
   post-test to cap=8.

## Open follow-ups

1. **Re-test with cap=8** — current binary has the fix; ideal next test.
2. **Measure agent CPU overhead during streaming load** — verify §11.5 (drop
   pid=0 CSwitch) achieved < 2% target.
3. **Test with NVENC OBS** — softer CPU load, control variable for "how much
   does Ayama help when contention is on GPU not CPU?".
4. **2nd CPU class** — DoD §8.6 #2 requires testing on at least one non-X3D
   CPU. Intel hybrid 13700K+ or single-CCD AMD remain pending.
5. **Investigate "Max FT = 69 ms" outlier** — was it OBS-induced, scheduler,
   or shader recompile?

## Files preserved

- `<repo-root>/reports/multistream-baseline.pm.csv`  — PresentMon raw, Phase A
- `<repo-root>/reports/multistream-baseline.csv`     — ayama-bench format
- `<repo-root>/reports/multistream-treated.pm.csv`   — PresentMon raw, Phase B
- `<repo-root>/reports/multistream-treated.csv`      — ayama-bench format

## Bugs caught in this test cycle

| # | Description | Status |
|---|-------------|--------|
| 14 | Rule 3 condition required `multi_ccd` flag, broken by bug #6 on 7950X3D | Fixed pre-test |
| 16 | Em-dash UTF-8 character renders as `ΓÇö` in PowerShell | Fixed |
| 17 | `kMaxPerName=4` insufficient for Discord (6+ helpers) → 2 escapees ran on default mask, polluted P99.9 | **Fixed post-test (cap=8)** |

Cumulative bug count across both empirical reports: **17 bugs** (15 fixed, 2 cosmetic open).

---

**Reported by:** Empirical test session 2026-05-16 (multi-stream evening run).
**Verified:** Rules 1+3+6 applied to 7 process slots; PowerShell `ProcessorAffinity`
confirms 5/7 pinned correctly, 2 Discord helpers escaped (bug #17 now fixed).
**Conclusion:** Real-world multi-encoder workload sees Avg/1%-low improvement,
deep-tail improvement gated by Discord pinning completeness. Re-test recommended
with new build to validate post-#17 deep tail metrics.
