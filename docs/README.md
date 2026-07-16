# Ayama empirical V-Cache study

This folder holds Ayama's empirical study: the test protocol and the per-game
A/B benchmark reports that measure what happens to game frame-time when a
CPU-bound game is **pinned to the V-Cache CCD** of an AMD Ryzen 9 7950X3D (and
the analogous policies for Intel hybrid / multi-CCD parts).

This is the project's most promising empirical direction — but the current
dataset is **provisional and under re-validation** (see the banner in
[`reports/EMPIRICAL_EVIDENCE_SUMMARY.md`](reports/EMPIRICAL_EVIDENCE_SUMMARY.md)),
and it is easily misread, so please read the scope notes below.

## What this study is

- **CPU-affinity tuning via documented OS APIs.** The only thing Ayama does to
  a game is set its CPU affinity (`SetProcessAffinityMask` on Windows) so the
  game's threads land on the V-Cache CCD. This is the same class of operation
  as Process Lasso or Task Manager's "Set affinity." **There is no code
  injection, no memory hooking, no reading or writing of another process's
  memory, and no driver.**
- **An honest A/B/A/B/A protocol.** Each test alternates Baseline (Ayama
  observing only, no policy) and Treated (policy active) phases on the same
  reproducible scenario, computes confidence intervals, and **reports null
  results as null** when the intervals overlap. See
  [`EMPIRICAL_TEST_PROTOCOL.md`](EMPIRICAL_TEST_PROTOCOL.md).

## What this study is NOT

- **Not anti-cheat work, and not anti-cheat evasion.** Games with kernel-mode
  anti-cheat (Vanguard, EAC, BattlEye, Ricochet, …) are **explicitly out of
  scope** — the protocol's §7.8 says to *skip them entirely* to avoid ban
  risk, not to circumvent them. The anti-cheat mentions in these documents are
  about *which games are excluded*, never about defeating anything. Every game
  measured here has no kernel anti-cheat in the tested mode.
- **Not a universal "Ayama makes games faster" claim.** The reports show the
  effect is real and meaningful on some titles (CPU-bound, cross-CCD-sensitive
  engines) and **statistically null on others** (well-threaded or GPU-bound
  engines). The honest, mixed result is the point.

## Scope and reproducibility caveats

- **One machine, one configuration.** All reports are from a single AMD Ryzen 9
  7950X3D + RTX 4090 box. The V-Cache effect is specific to AMD X3D multi-CCD
  parts; other hardware will behave differently.
- **Raw capture data is not in git history.** The reports summarize ~65 MB
  of raw PresentMon-style CSV captures under `reports/raw_data/`. The files
  are present in the maintainer's working tree (imported at the 2026-07-16
  separation) but the path is **git-ignored** (`.gitignore` at the project
  root) to keep the repo lean; if they should be published for independent
  re-analysis, the right vehicle is Git LFS or a release asset.
- **The current dataset is provisional, not citable.** Some runs were invalid
  (captured with a since-fixed test-time code bug, or with the game GPU-bound,
  which masks the CPU-affinity effect), and the raw CSV captures were created
  then **rewritten** — so the reports cannot be traced to original, untouched
  captures. The *direction* of the finding was observed directly (e.g. Fallout 4,
  Halo 2), but the specific magnitudes are **pending a clean re-run** under the
  corrected protocol before they can be cited.

## Files

- [`EMPIRICAL_TEST_PROTOCOL.md`](EMPIRICAL_TEST_PROTOCOL.md) — the reproducible
  measurement procedure (v2.0, 5-run A/B/A/B/A).
- [`reports/EMPIRICAL_EVIDENCE_SUMMARY.md`](reports/EMPIRICAL_EVIDENCE_SUMMARY.md)
  — the cross-game summary table (where the effect shows, where it's null).
- `reports/*.md` — the individual per-game reports (Halo MCC catalog, RDR2,
  Fallout 4, Hogwarts Legacy, Borderlands 2, Minecraft variants, and a
  CPU-overhead optimization study).
- `reports/crimson-desert_2026-05-19_external.md` — an **external contributor**
  submission on an **unknown machine**. Kept separate from the summary above
  (which is hardware-locked to the 7950X3D) and **not citable** until the
  hardware/scene are confirmed: NULL on means, with a small-sample
  max-frame-time reduction and treated-run variance reduction.

> **Historical note.** This study was run during the project's pre-rebrand
> *gamma* era and predates the `v0.1.0-experimental` reset. It is evidence of
> method and findings, not a current product claim. The project's planning
> archive ([`docs/plans/history/`](plans/history/)) cross-references
> this study as the empirical basis for Ayama's design.
