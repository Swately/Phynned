# Scenario: Crimson Desert — external contributor, 5-run A/B/A/B/A (n=3 baseline / n=2 treated)

> **⚠️ External contribution — hardware NOT yet reported.** This run was
> provided by a collaborator on a different machine than the project's
> reference box (AMD Ryzen 9 7950X3D + RTX 4090). **The CPU/GPU/OS and the
> game settings/scenario are not yet known.** Because Ayama's whole mechanism
> is pinning the hot thread to the **3D V-Cache CCD on AMD X3D parts**, this
> result cannot be interpreted — or compared to the reference reports — until
> we know whether the test machine is an X3D CPU at all. It is therefore
> **kept separate from** [`EMPIRICAL_EVIDENCE_SUMMARY.md`](EMPIRICAL_EVIDENCE_SUMMARY.md)
> (which is hardware-locked to the 7950X3D) until the hardware is confirmed.

## Status

**NO STATISTICALLY DETECTABLE DIFFERENCE on the mean metrics** (Avg / P99 /
P99.9 / hitches all fall within the confidence interval). The **only**
significant signal is **max frame time: treated 30.92 ms vs baseline 37.37 ms
(−17.3%, treated < baseline, CIs do not overlap)**, accompanied by a large
**run-to-run variance reduction** in the treated group across every metric.

This is the same *"null mean, but spike reduction + determinism"* signature
seen on the reference box for Halo Reach MCC and Borderlands 2 — but here it
is on a **small sample (n=3 / n=2) and unknown hardware**, so it is **not**
evidence of an FPS benefit. It is, at most, "worth a longer, characterized
re-test."

## Raw `ayama-cli bench multi` output (verbatim, as submitted)

```
Loading baseline group (3 files):
  [OK] run1.bench.csv — avg=13.99 P99=19.77 P99.9=28.15 hitches=158
  [OK] run3.bench.csv — avg=14.45 P99=28.81 P99.9=31.29 hitches=310
  [OK] run5.bench.csv — avg=15.53 P99=29.67 P99.9=32.60 hitches=580
Loading treated group (2 files):
  [OK] run2.bench.csv — avg=14.07 P99=25.58 P99.9=29.07 hitches=217
  [OK] run4.bench.csv — avg=14.47 P99=27.88 P99.9=29.56 hitches=323

-- ayama-bench multi-run aggregate -----------------------------
  Baseline: n=3 runs   Treated: n=2 runs

  Frame time (lower = better)
  Metric    Baseline mean    Treated mean    Delta    Significance
  Avg          14.65 ± 0.90      14.27 ± 0.39    +2.6%  WITHIN CONFIDENCE INTERVAL
  P99          26.08 ± 6.20      26.73 ± 2.25    -2.5%  WITHIN CONFIDENCE INTERVAL
  P99.9        30.68 ± 2.59      29.32 ± 0.47    +4.5%  WITHIN CONFIDENCE INTERVAL
  Max          37.37 ± 4.31      30.92 ± 0.13   +17.3%  SIGNIFICANT (treated < baseline)

  Stutters (lower = better)
  hitch>16.67  349.33 ± 241.86   270.00 ± 103.88  +22.7%  WITHIN CONFIDENCE INTERVAL

  Per-run dispersion (high stddev = noisy test):
  Avg   stddev: baseline=0.79 ms  treated=0.28 ms
  P99   stddev: baseline=5.48 ms  treated=1.62 ms
  P99.9 stddev: baseline=2.29 ms  treated=0.34 ms
  Max   stddev: baseline=3.81 ms  treated=0.09 ms
  Hitch stddev: baseline=213.7    treated=75.0

  Verdict (multi-run, statistical):
  NO STATISTICALLY DETECTABLE DIFFERENCE (CIs overlap)
```

(The original submission used absolute `%TEMP%` paths containing the
contributor's Windows username; those have been trimmed to the file names to
avoid publishing a third party's identity.)

## What the data honestly shows

- **Means: no detectable effect.** Avg ≈14.5 ms (~69 FPS), P99 ≈26 ms, P99.9
  ≈30 ms — baseline and treated overlap within their confidence intervals.
  At ~69 FPS the test may simply not be CPU-bound (GPU-bound or well-threaded
  → nothing for a CCD pin to fix), but we cannot tell without the hardware,
  the GPU-utilisation trace, and the scene.
- **Max frame time: real, significant reduction** (37.4 → 30.9 ms, −17.3%),
  and the treated max is almost perfectly repeatable (stddev 0.09 ms vs
  3.81 ms). If this holds up, it is the familiar *worst-frame-spike
  elimination* pattern (a pinned thread stops getting migrated mid-frame).
- **Strong variance reduction in treated, every metric:** Avg stddev
  0.79→0.28, P99 5.48→1.62, P99.9 2.29→0.34, Max 3.81→0.09, hitches
  213.7→75.0. Treated runs are far more *consistent* — the determinism
  signature. But with only **2 treated runs**, a low stddev is weak evidence
  (two points are easy to land close together by chance).

## Caveats (why this is not yet citable)

1. **Hardware unknown.** The entire V-Cache thesis depends on the CPU being an
   AMD X3D part with two CCDs. On a non-X3D AMD, an Intel hybrid, or a
   single-CCD CPU, Ayama applies a *different* policy (or none), so the
   result would mean something else entirely. **Needed: CPU, GPU, OS.**
2. **Tiny sample (n=3 / n=2).** The tool's own dispersion note says "consider
   more runs." A clean characterization needs the full 5-run A/B/A/B/A (3+2 is
   the minimum) re-run, ideally several times.
3. **Scene / settings unknown.** Crimson Desert (Pearl Abyss, 2025) is a
   modern AAA title; whether the captured scene was CPU-bound or GPU-bound is
   the single biggest determinant of whether Ayama can do anything (see the
   RDR2 GPU-bound-vs-CPU-bound reversal in the reference reports). No
   GPU-utilisation figure was provided.
4. **The headline tool verdict is NULL.** Per the multi-run aggregate,
   "NO STATISTICALLY DETECTABLE DIFFERENCE." We do not upgrade that to a win
   on the strength of a 2-run max-frame-time delta.

## What we need to complete this report

- [ ] **Hardware**: exact CPU (is it X3D?), GPU, RAM, OS build.
- [ ] **Ayama build/version** used, and which policy it applied (the UI's
      "Version info" panel + the active policy name).
- [ ] **Scene + settings** and a **GPU-utilisation reading** during capture
      (to know CPU-bound vs GPU-bound).
- [ ] **A full re-run** (5-run A/B/A/B/A, more reps if possible) so the
      variance-reduction signal is backed by more than 2 treated points.

Once the hardware is confirmed (and if it is an AMD X3D part), this can be
folded into the main evidence summary under the appropriate tier — most
likely **"NULL with variance reduction"** unless a CPU-bound scene re-test
moves the means.

---

**Reported by**: external collaborator (machine + scenario pending).
**Recorded**: 2026-05-19, verbatim from the submitted `ayama-cli bench multi`
output. **Conclusion**: null on means; a real but small-sample max-frame-time
reduction and treated-run determinism. **Not citable as a benefit** until
hardware, scene, and a larger re-run are provided.
