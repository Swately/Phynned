<!-- Ayama container-separation — risk register (the pre-commit gate). -->
<!-- Every plausible failure mode of the migration, its mitigation as concrete action, -->
<!-- and its first-hand verification. Nothing ships while a row is `open`. -->

# Ayama Separation — Risk Register

Status: `measured` (Phases 1-5 executed 2026-07-16; every row terminal except `DR3`,
which closes at the operator-gated Phase 7) · Tier **2** per
[`PLAN_TIER_PROTOCOL`](../../../../protocols/quality/PLAN_TIER_PROTOCOL.md) §1.1 trigger 4.
Siblings: [`AYAMA_SEPARATION_MASTER_PLAN.md`](AYAMA_SEPARATION_MASTER_PLAN.md) (the why) ·
[`AYAMA_SEPARATION_IMPLEMENTATION_STRATEGIES.md`](AYAMA_SEPARATION_IMPLEMENTATION_STRATEGIES.md)
(the how; each edit site cites the risk ID it mitigates).

**The gate (§3.3):** this migration MUST NOT be committed, and `apps/ayama` MUST NOT be removed,
while any row below is `open`. `mitigated` requires the Verification column to have been **run**,
not assumed.

**Why this change is Tier-2 at all.** The container has **no git** (standing rule 2 — git left
`G:\phyriad` on 2026-07-15). A botched move is not recoverable with `git checkout`; the only net
is the restic backup (`G:\phyriad-backup`, latest snapshot `f59e4335`). Every data-loss row below
is written against that fact.

---

## The rows

### `DR1` — the original is destroyed before the copy is proven

| Field | Content |
|---|---|
| **Class** | Data loss / irreversibility |
| **Failure mode** | The migration is executed as a *move* (`robocopy /MOVE` or `mv`) of `apps/ayama` → `projects/Ayama`, or `apps/ayama` is deleted at the end of a phase that later turns out to be broken. With no git in the container, the operator's invested work (111 files) is recoverable only from restic. Site: `AYAMA_SEPARATION_IMPLEMENTATION_STRATEGIES.md` S1, S8. |
| **Mitigation** | **Copy, never move** — S1 runs `robocopy … /E /XD build` with no `/MOVE`. `apps/ayama` stays byte-for-byte intact through Phases 1-6. The removal is isolated to Phase 7 (S8), which is **operator-gated** and runs only after the Phase 5 decoupling proof is green. The same rule binds the doc import in S5: `catalog/cpp/docs/apps/ayama/` and `…/plans/archive/ayama/` are **copied**. Until Phase 7, the entire migration is undone by deleting `projects/Ayama`. |
| **Verification** | After S1: `apps/ayama` still present, recursive file count unchanged. Before S8: Phase 5 green **and** the operator's explicit go recorded here. |
| **Status** | `mitigated` — **run 2026-07-16:** S1 executed as `robocopy /E /XD build` (no `/MOVE`); `apps/ayama` verified intact after the copy (111 files, unchanged); every subsequent edit landed in `projects/Ayama` only. The Phase-7 removal remains operator-gated and has NOT run. |

### `DR2` — the copy silently drops files

| Field | Content |
|---|---|
| **Class** | Data loss |
| **Failure mode** | `robocopy` skips files (long paths, locked handles, an over-broad `/XD`), and the loss goes unnoticed because a build can be green while a doc, script, or manifest is missing. Site: S1. |
| **Verification** | Recursive file count + total size, source vs destination, excluding `build/`. Design-time source: **111 files, 1.2 MB**. A mismatch halts Phase 1. Robocopy's exit code is checked (≥8 = failure; 0-7 are success codes — a naive `if errorlevel 1` misreads it). |
| **Mitigation** | The count comparison is the gate, run as part of Phase 1, not after. |
| **Status** | `mitigated` — **run 2026-07-16:** robocopy exit **3** (<8 = success); source 111 files = destination 111 files (excluding `build/` and the pre-existing plan triad). |

### `DR3` — a half-migrated container: two Ayamas, or none

| Field | Content |
|---|---|
| **Class** | Data loss / irreversibility (single-source-of-truth break) |
| **Failure mode** | The 7 container touchpoints (master plan §7) are updated at a different time than the removal of `apps/ayama`. Update early → `.clangd`, `package_release.ps1`, `CODEOWNERS`, `INVENTORY.md` and the corpus extractor point at a project that is not finished. Update late → they point at a directory that no longer exists. Between Phases 1 and 7 **two copies of Ayama exist**, and an edit landing in the wrong one is silently lost. Site: S8. |
| **Mitigation** | The touchpoints and the removal land in **one** change (S8, BOOT §3 rule 4). During Phases 1-6 `projects/Ayama` is the *only* copy under edit — `apps/ayama` is frozen the moment S1's copy completes, and no edit is made there for any reason. If a defect is found in `apps/ayama` during the window, it is fixed in `projects/Ayama` only. |
| **Verification** | After S8: grep the container for `apps/ayama` → zero hits outside `legacy/` and the docs that reference *Ayama the project* rather than the path. `scripts/check_inventory.py` passes. `scripts/gen_clangd.py` regenerated and its output contains no `apps/ayama` path. |
| **Status** | `open` — **the two-copies window is open as of 2026-07-16** (Phases 1-5 done, Phase 7 not run). The freeze discipline is in force: `apps/ayama` untouched since the S1 copy; every edit landed in `projects/Ayama`. Closes when Phase 7 lands the 7 touchpoints + the removal in one change. |

### `DR4` — the decoupling proof breaks other projects

| Field | Content |
|---|---|
| **Class** | Data loss / irreversibility |
| **Failure mode** | S6 renames `catalog/cpp` → `catalog/cpp__hidden` to prove Ayama no longer needs it. `catalog/cpp` is the **live C++ substrate** — every other unit in the container builds against it. If the session dies, is interrupted, or the build throws mid-window, the substrate stays hidden and every other build in the container breaks. Site: S6. |
| **Mitigation** | The restore is unconditional — it runs on the failure path too, not only on success. The window is seconds-to-minutes and no other build runs inside it. The name `catalog/cpp__hidden` is deliberately conspicuous: if it exists, the restore did not run, and renaming it back is the whole recovery. Documented in S6 so any later session can undo it without this context. |
| **Verification** | After S6, `catalog/cpp` exists at its original path and `ls catalog/cpp` matches its pre-window listing. |
| **Status** | `mitigated` — **eliminated by a design change (2026-07-16, notified to the operator):** the decoupling proof was redesigned from *rename `catalog/cpp` aside* to *build an isolated copy of the project in a scratchpad directory where `../../catalog/cpp` does not resolve* (verified: `Test-Path ..\..\catalog\cpp` = False there). Same detection power — any residual external reference fails the build — with the substrate never touched. The rename window this risk described no longer exists in the procedure. |

### `DR5` — vendoring drags 275 MB of FG bench artifacts

| Field | Content |
|---|---|
| **Class** | Data loss (by dilution) / irreversibility |
| **Failure mode** | `catalog/cpp/render` measures 276 MB, of which `render/vulkan/bench` alone is **275 MB** (FG bench artifacts from the PhyriadFG lineage, no Ayama consumer). Vendoring it wholesale makes Ayama's first git commit 276 MB — effectively permanent, since git history cannot be trimmed without a rewrite. Site: S2. |
| **Mitigation** | Vendor `render/vulkan/{src,include,shaders,CMakeLists.txt}` only; `bench/` is excluded. If `render/vulkan/CMakeLists.txt` adds `bench/` unconditionally, guard it with `if(EXISTS …)` rather than vendoring the directory. `.gitignore` (S7) covers `build/`, `build-release/`, `_deps/`. |
| **Verification** | Before the S7 `git init`: `du -sk projects/Ayama` is on the order of PhyriadFG's vendored framework (480 KB) plus Ayama's own 1.2 MB — **not** hundreds of MB. `git status` shows no `.exe`/`.obj`/`_deps` staged. |
| **Status** | `mitigated` — **run 2026-07-16:** `framework/` measured **2,210 KB** across the 17 units; zero `bench/` directories inside the vendored `render/` (verified by recursive search); `.gitignore` written covering `build/`, `build-release/`, `_deps/`, release zips. The final `git status` check runs at the operator-gated Phase 6 `git init`. |

### `SR1` — the substrate forks (the accepted cost)

| Field | Content |
|---|---|
| **Class** | Dogma (D-16, single source of truth) |
| **Failure mode** | Vendoring copies 13+ pillars into `projects/Ayama/framework/`. From that moment a fix in `catalog/cpp` does not reach Ayama, and a fix in Ayama's copy does not reach the substrate. The two drift, silently, forever. This is a **certainty, not a possibility**. |
| **Mitigation** | **None — this risk is accepted, not mitigated.** The operator chose vendoring on 2026-07-16 when presented with the coupled alternative and this exact cost stated. It is the same cost `projects/PhyriadFG` already carries (it vendored 4 pillars and has diverged since). The rationale: a project that cannot build outside the container is not an independent project, which was the goal of the separation (master plan §2). The residual is stated plainly rather than hidden: **Ayama's `framework/` is a snapshot, and nobody is watching the drift.** |
| **Verification** | n/a — accepted. The vendored tree records its provenance: a `framework/VENDORED.md` naming the source (`catalog/cpp`), the date, and the pillar list, so a future session can diff against the substrate deliberately. |
| **Status** | `accepted` — by the operator, 2026-07-16 |

### `TR1` — the false green survives the migration

| Field | Content |
|---|---|
| **Class** | Dogma (verify-before-claim / honest reporting, CONDUCT §2-§3) |
| **Failure mode** | `testbench.ps1` prints `[ayama] OK` while ctest prints `No tests were found!!!` and exits **0** — **measured 2026-07-16, this is the current behavior**, not a hypothetical. The root cause is a missing `include(CTest)` at `CMakeLists.txt` top level → no root `CTestTestfile.cmake` → ctest discovers nothing → the script's `$LASTEXITCODE` check passes. If it is carried into `projects/Ayama` unchanged, the separated project ships a testbench that certifies green having run nothing, and this migration's own Phase 3/5 gates become meaningless. |
| **Mitigation** | S3: add `include(CTest)` after `project()`; pass `-DAYAMA_BUILD_TESTS=ON` (the script currently passes `-DPHYRIAD_BUILD_TESTS=ON`, which Ayama's CMake does not read — tests built only because the option defaults `ON`); and **assert a non-empty test set** via `ctest -N`, exiting `4` on `NO_TESTS_DISCOVERED` rather than trusting ctest's exit code. |
| **Verification** | `ctest --test-dir build -N` reports a non-empty set, and `.\testbench.ps1` on a tree with a deliberately broken test **fails** (a negative control — a green that cannot go red is not a gate). |
| **Status** | `mitigated` — **run 2026-07-16:** `include(CTest)` added at the root; the testbench now passes `-DAYAMA_BUILD_TESTS=ON` and refuses an empty set (`NO_TESTS_DISCOVERED`, exit 4). Discovery went from **0 → 49 tests** (Ayama's 10 + the vendored pillars' suites; the plan's "10" predates vendoring). **Negative control run:** an isolated copy with `assert(expired == 999u)` deliberately injected → testbench printed `TESTS_FAIL`, exit **3**, naming `ayama_learn_test` — the gate goes red. |

### `TR2` — the time-bomb test, and the ones like it

| Field | Content |
|---|---|
| **Class** | Dogma (verify-before-claim) |
| **Failure mode** | `learn/tests/learn_test.cpp:251` `assert(expired == 1u)` aborts with **exit 127** — measured 2026-07-16. The test seeds a "fresh" entry at the hardcoded `"2026-05-14T12:00:00Z"`; `PerGameMemory::needs_revalidation` (`learn/src/PerGameMemory.cpp:646-671`) measures age against **today** (`GetLocalTime`). The entry crossed the 30-day window on ~2026-06-13 and both entries now expire → `expired == 2`. Merely bumping the date reintroduces the bomb with a new fuse. |
| **Mitigation** | S4: derive the fresh timestamp from `GetLocalTime` at test time so the test cannot age out. The stale entry keeps its `"2020-01-01"` — a fixed past date can only age deeper into a >30-day window, never out of it. |
| **Verification** | `ayama_learn_test.exe` exits **0** (currently 127) — run on the day of the fix **and** confirmed date-independent by reasoning about the fix, not by waiting. Then the full set: 10 discovered, **10/10 passing**. |
| **Residual, stated honestly** | This register fixes the one time-bomb that fired. It does **not** claim the other 9 tests are free of the same pattern, and it does not close the coverage gap: `config/` has an `add_test` (`config/CMakeLists.txt:47`) behind `if(EXISTS tests/config_test.cpp)` guarding a source that **has never existed**. |
| **Status** | `mitigated` — **run 2026-07-16:** the fresh timestamp is now derived from `std::time`/`localtime` at test time (portable, one path both platforms); `ayama_learn_test` exits **0** (was 127) and passes in both the in-place and the isolated from-scratch builds. Date-independence holds by construction: the derived date is always "today"; the stale `"2020-01-01"` can only age deeper into the >30-day window. The residual above stands. |

### `BR1` — the vendored closure is wrong

| Field | Content |
|---|---|
| **Class** | Crash (build/link failure at the new location) |
| **Failure mode** | The closure in master plan §4 is grep-derived. If a pillar is missed, `ensure_phyriad_target`'s documented failure mode bites: an **undefined target silently degrades to a raw `-l` flag, so configure passes while the includes are absent** (`apps/ayama/CMakeLists.txt:78-79`, the file's own comment recording that this already happened on 2026-07-15 for `process`/`etw`/`stigmergy`/`ipc`). A configure-only check would report success on a broken tree. |
| **Mitigation** | S2: the gate is a **full from-scratch build + link**, never configure alone. `runtime`/`orchestration`/`behavior`/`correlation`/`profile` are omitted initially (zero `#include` sites); a link error names what is missing, it is vendored, and the master plan §4 finding is corrected in the same change. |
| **Verification** | S6's decoupling proof — a green from-scratch build somewhere `catalog/cpp` cannot resolve. That is the only build that can distinguish a vendored closure from `../..` still resolving. |
| **Status** | `mitigated` — **run 2026-07-16:** the closure GREW during implementation exactly as this row anticipated — `phyriad_ui` hard-links `phyriad_runtime` (→ `graph`, `profile`, `scheduler`), settling at **17 units**; `orchestration`/`behavior`/`correlation` stayed out and the link is green without them. Proof: from-scratch configure + build + **49/49 tests** in an isolated directory where `Test-Path ..\..\catalog\cpp` = **False**. Bonus finding: `ayama-ui.exe` had been silently skipped in every standalone build since the root CMakeLists retired (its guard needs `imgui_lib`); the separation restored it via a root-level GLFW/ImGui FetchContent — the dist now carries all 5 executables. |

---

## Summary

| ID | Class | Status |
|---|---|---|
| `DR1` | data-loss | `mitigated` (2026-07-16 — copy verified, original intact; Phase-7 removal still operator-gated) |
| `DR2` | data-loss | `mitigated` (111 = 111, robocopy exit 3) |
| `DR3` | data-loss | **`open`** — closes at Phase 7 (touchpoints + removal in one change); freeze discipline in force |
| `DR4` | data-loss | `mitigated` (eliminated by design — isolated-copy proof; substrate never touched) |
| `DR5` | data-loss | `mitigated` (framework/ = 2,210 KB; zero bench/; .gitignore in place) |
| `SR1` | dogma (D-16) | `accepted` — operator, 2026-07-16 (framework/VENDORED.md records provenance) |
| `TR1` | dogma (conduct) | `mitigated` (0 → 49 discovered; negative control went red, exit 3) |
| `TR2` | dogma (conduct) | `mitigated` (derived date; exit 127 → 0; residual coverage gap stands) |
| `BR1` | crash | `mitigated` (17-unit closure linker-settled; isolated build 49/49 green) |

7 `mitigated`, 1 `accepted`, **1 `open` (`DR3`)** — which is why Phase 7 (removal of
`apps/ayama` + the 7 container touchpoints) and the Phase 6 initial commit both wait for the
operator's explicit go. Risks discovered during implementation were added in place — the
register stays living until the migration lands.

---

## Round 2 — adversarial verification (2026-07-16, 5 independent lenses)

A 5-agent adversarial pass (closure · copy-fidelity · docs · register-honesty · build-truth),
each instructed to REFUTE the migration's claims. **2 lenses CLEAN** (register honesty —
every `mitigated` row re-verified first-hand including a live test run; build truth — the
in-place tree re-built green, all 5 dist binaries fresh). **8 findings, all dispositioned:**

| Finding | Severity | Disposition |
|---|---|---|
| `tools/installer/install.bat:39-51` — developer-install tiers pointed 4 levels up at the CONTAINER's retired root-build layout (`G:\phyriad\build{,_ayama}\ayama\…`, dead paths today; a stale container build reappearing there would have been silently installed) | **major** | **FIXED** — tiers re-pointed at the project's real layouts (`..\..\build{,-release}\ayama-dist\…`); ZIP same-dir fallback kept |
| `README.md` — headline/table/citation presented the 14-test dataset as "statistically rigorous empirical validation … 95% CI" while its OWN linked evidence docs banner it "provisional, must NOT be cited as validated, pending a clean re-run" | **major** | **FIXED** — every claim re-calibrated (headline, TL;DR banner added, comparison table, CPU table, citation note); the front door now carries the same status its evidence docs mandate |
| `docs/README.md` claimed raw captures "not in the repository / git-ignored" while 65 MB of `raw_data/` CSVs sat in the tree with no gitignore entry (would have landed in the initial commit — a `DR5`-class miss) | minor | **FIXED** — `docs/reports/raw_data/` added to `.gitignore`; the paragraph amended to match reality |
| `docs/ARCHITECTURE.md` understated the coverage gap: `ipc/` also has zero tests, not just `config/` | minor | **FIXED** — both gaps named (6 of 8 modules covered) |
| `README.md` source-layout enumeration omitted `learn/` and `config/` | minor | **FIXED** — all 8 modules listed |
| Stale `apps/ayama` comments (root `CMakeLists.txt`, `config/CMakeLists.txt:13,16`) | minor | **FIXED** — comments updated to the self-contained identity |
| Vendored `render/composite/CMakeLists.txt:32,35` + `render/opengl3/CMakeLists.txt:15` standalone-as-root guards have broken path arithmetic — byte-identical to the `catalog/cpp` original (pre-existing catalog defect, unreachable in Ayama's build) | minor | **RECORDED, not fixed** — documented in `framework/VENDORED.md` as inherited (keeps the diff-vs-catalog clean); flagged upstream for the container |
| Freeze-wording imprecision: `apps/ayama/build/` was regenerated at 16:42 on migration day — by THIS migration's own Phase-0 state-survey testbench run, 5 minutes BEFORE the S1 copy. Sources: hash-verified untouched (111/111, zero unexpected diffs, exactly the 7 intended files differ — in `projects/Ayama` only) | minor | **WORDING CORRECTED here:** the freeze claim covers *sources*; the original's `build/` artifacts were consumed by the pre-copy state survey, which is when the false-green/time-bomb evidence was gathered. No pre-existing build artifact had evidentiary value beyond that run. |

Verification note: the copy-fidelity lens could not do a *cryptographic* pre-migration
baseline (no container git; the restic passphrase lives in the operator's password manager)
— the freeze is evidenced by NTFS timestamps + the 111/111 hash identity between the two
trees, which is consistent but not a pre-image proof. Declared per CONDUCT §3.4.

<!-- Made with my soul - Swately <3 -->
