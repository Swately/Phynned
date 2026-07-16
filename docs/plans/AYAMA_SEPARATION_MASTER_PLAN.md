<!-- Ayama container-separation — master plan (design/explanation). -->
<!-- Why: the contract a reader acts on before the migration runs. The sibling -->
<!-- strategies doc holds the "how"; the risk register holds the safety gate. -->

# Ayama Separation — Master Plan

**Tier: 2** (risk-bearing) per [`PLAN_TIER_PROTOCOL`](../../../../protocols/quality/PLAN_TIER_PROTOCOL.md)
§1.1 trigger 4 (data-loss / irreversibility: a migration whose final step deletes the
original, in a container with **no git**). Status: `measured` — **Phases 1-5 executed and
verified 2026-07-16** (the living status per risk is the register's; Phases 6-7 await the
operator's go). Type: **Planning / design (Explanation)** per
[FDP §3](../../../../protocols/quality/FORMAL_DOCUMENT_PROTOCOL.md).
Deltas design→reality: the closure grew to **17 units** (`runtime`+`profile` via `phyriad_ui`,
see the strategies S2 outcome note); the Phase-3 gate metric grew from "10 tests" to **49**
(the vendored pillars' suites joined discovery); the Phase-5 proof was redesigned to an
isolated-copy build (`DR4` eliminated by design); `ayama-ui.exe` — silently skipped in every
standalone build since the root CMakeLists retired — was restored via a root-level GLFW/ImGui
FetchContent.

Siblings — the three documents are one set:
[`AYAMA_SEPARATION_IMPLEMENTATION_STRATEGIES.md`](AYAMA_SEPARATION_IMPLEMENTATION_STRATEGIES.md)
(the "how", per-phase edit sites) ·
[`AYAMA_SEPARATION_RISK_REGISTER.md`](AYAMA_SEPARATION_RISK_REGISTER.md) (the pre-commit gate).

**Authorization record.** The container's standing rule 3 reserves relocation of `projects/`
children to the operator. This migration runs under the operator's explicit instruction
(2026-07-16: *"verifica el estado del proyecto de Ayama y sepáralo en G:\Phyriad\projects
usando de referencia la estructura de G:\Phyriad\projects\PhyriadFG"*), and the coupling model
below was chosen by the operator when presented with three options.

---

## §1 — What is being moved, and what Ayama is

**Ayama** is a non-invasive runtime optimizer for asymmetric multi-CCD and hybrid CPUs: it
detects a game process via ETW (read-only), pins it to the best cores available (V-Cache CCD on
AMD X3D, P-cores on Intel hybrid), raises scheduler priority, and reverts on exit. No DLL
injection, no process-memory access, no hooking — only `SetProcessAffinityMask`,
`SetPriorityClass`, `SetThreadAffinityMask` (`legacy/POR_QUE_AYAMA.md` [V1], the operator's own
scoping statement).

Today it lives at `apps/ayama/` (111 files, 1.2 MB): eight library submodules (`observer`,
`learn`, `policy`, `action`, `bench`, `ipc`, `config`, `core`), four tools (`ayama-agent`,
`ayama-cli`, `ayama-bench`, `ayama-ui`) and an installer. Its docs live **elsewhere** — 17 files
at `catalog/cpp/docs/apps/ayama/` and 10 planning docs at
`catalog/cpp/docs/plans/archive/ayama/`.

**Naming note (resolved 2026-07-16).** "Ayama" names two distinct things in the container's
history: this CPU optimizer, and — per
`catalog/cpp/docs/plans/archive/AYAMA_LAYERED_FG_MASTER_PLAN.md:8` [V1] — the multi-GPU
frame-gen assistant at `apps/render_assistant/`. That second line has since been renamed
**PhyriadFG** and already separated to `projects/PhyriadFG`. **In this document and in the
separated project, "Ayama" means the CPU optimizer, unqualified.** The FG line is out of scope.

## §2 — Why separate (the goal)

`G:\phyriad` is a development-environment container hosting n independent projects
(BOOT §1). Ayama is one of those projects, not a part of the container: it has its own product
identity, its own license (PolyForm Noncommercial 1.0.0), its own release workflow and its own
end users. It currently sits in `apps/` — a container-owned area — which makes it
indistinguishable from container scaffolding and leaves its docs stranded three directories away
in the C++ substrate's doc tree.

The target end-state is **structural parity with `projects/PhyriadFG`**: a self-contained
project directory that builds with no reference outside itself, carries its own git repository,
and documents itself in its own `docs/`.

## §3 — The measured starting state (verified first-hand 2026-07-16)

Everything in this section was run or read, not recalled.

| Fact | Evidence |
|---|---|
| Build is **green** standalone | `apps/ayama/testbench.ps1` → 192 targets, incl. `ayama-ui.exe` + `runtime/{ayama-agent,ayama-cli,ayama-bench,ayama-service-register}.exe` |
| `testbench.ps1` reports a **FALSE GREEN** | it printed `[ayama] OK` while ctest printed `No tests were found!!!` and exited **0**; the script only checks `$LASTEXITCODE` |
| Root cause of the false green | `apps/ayama/CMakeLists.txt` never calls `enable_testing()` / `include(CTest)` → no `build/CTestTestfile.cmake` is generated → ctest discovers nothing. This is what `catalog/cpp/APPS_TESTBENCH_REPORT.md:10` (`ayama \| NO_TESTS … Zero add_test at app level`) meant — the report is **correct in effect**. |
| **10** tests are registered in submodules | 11 `add_test` lines; `ayama_config_test` sits behind `if(EXISTS tests/config_test.cpp)` and that source does not exist → never built |
| Run by hand: **9 pass, 1 fails** | `ayama_learn_test.exe` aborts, **exit 127**, `learn/tests/learn_test.cpp:251`, `assert(expired == 1u)` |
| The failure is a **time-bomb**, not a regression | the test seeds a "fresh" entry at `last_validated = "2026-05-14T12:00:00Z"` and expects only the 2020 entry to expire under a 30-day window; `PerGameMemory::needs_revalidation` (`learn/src/PerGameMemory.cpp:646-671`) compares against **today** via `GetLocalTime`. On 2026-07-16 the "fresh" entry is ~63 days old → both expire → `expired == 2`. It passed when written in May and aged out unnoticed behind the false green. |

**Pre-existing breakage, independent of this migration** (already broken today):
`apps/ayama/README.md` links `../../{README,LICENSE,FAQ,ARCHITECTURE}.md` and `../../docs/…`
— none of those targets exist at the container root · `tools/ayama-ui/build.bat:27` sets
`FRAMEWORK=%ROOT%\framework` — that directory is gone · `tools/ayama-ui/ReportExporter.hpp:51-53`
hardcodes a runtime path `..\..\..\..\docs\ayama\reports` — gone · the `.github/workflows/*`
invoke `-DPHYRIAD_BUILD_AYAMA=ON` against a root `CMakeLists.txt` retired to
`legacy/root_cmake/` on 2026-07-15.

## §4 — The architecture decision: vendoring (operator-chosen)

**Decision: vendor.** `projects/Ayama/framework/` holds Ayama's own copy of the pillars it
consumes; `PHYRIAD_ROOT` is re-pointed from `${CMAKE_CURRENT_SOURCE_DIR}/../..` to
`${CMAKE_CURRENT_SOURCE_DIR}/framework`. This is exactly PhyriadFG's model
(`projects/PhyriadFG/CMakeLists.txt:25`: `set(_fw "${CMAKE_CURRENT_SOURCE_DIR}/framework")`,
22 files, 480 KB vendored, zero references to `catalog/cpp` [V1]).

**Why the alternative was rejected.** `PHYRIAD_ROOT = ../..` resolves correctly from
`projects/Ayama` (same depth as `apps/ayama` — verified), so a coupled move is free. But a
coupled Ayama does not build outside the container and is therefore a *relocated folder*, not an
independent project — it would not meet §2's goal.

**The dependency closure (verified by include-grep + CMake `target_link_libraries`).**
The CMakeLists ensures 16 pillars; the code does not use all of them.

- **Directly included by Ayama's code:** `hal` (17 sites), `schema` (13), `topology` (8),
  `ui` (6), `tuning` (5), `process` (4), `api` (4), `stigmergy` (2), `node` (1), `ipc` (1),
  `etw` (1), plus `<phyriad/version.hpp>` (from `_meta`).
- **Pulled in transitively:** `phyriad/api/` **lives inside `graph/include/`** — so the four
  `api/*` headers Ayama uses drag in `graph` + `scheduler` + `node` + `schema` + `hal`.
  `transport` arrives via `node` and `stigmergy`. `ui` links `render`, `render_opengl`,
  `render_vulkan`, `ui_compiled`.
- **Ensured but never included by any Ayama source:** `runtime`, `orchestration`, `behavior`,
  `correlation` (and `profile`, reachable only through `runtime`). Note `catalog/cpp/CANON.md`
  names Ayama as the consumer of `behavior` (PressureScore → differential-pin) and
  `correlation` — the grep contradicts that. **This document does not claim those pillars are
  dead**; it claims Ayama's sources contain zero `#include <phyriad/{behavior,correlation}/…>`
  as of 2026-07-16. The doc/code drift is recorded, not resolved here.

**The closure is not settled by grep — the linker is the authority.** Phase 2 vendors the
grep-derived closure and lets a from-scratch build prove it; a missing pillar surfaces as a
link error, and the closure grows until green. `catalog/cpp/hal` is the leaf (no dependencies).

**The 276 MB question.** `catalog/cpp/render/` measures 276 MB, but `render/vulkan/bench` alone
is 275 MB (FG bench artifacts, the PhyriadFG lineage). The library proper —
`render/vulkan/{src,include,shaders}` + `render/present` + `render/opengl3` — is ~440 KB.
`bench/` is **not** vendored.

**The accepted cost.** Vendoring forks the substrate: a fix in `catalog/cpp` no longer reaches
Ayama on its own. This is a real, permanent cost, accepted by the operator, and it is the cost
PhyriadFG already lives with. Tracked as `SR1` in the risk register.

## §5 — The target structure (PhyriadFG parity)

```
projects/Ayama/
  CMakeLists.txt          PHYRIAD_ROOT → ./framework
  LICENSE                 PolyForm Noncommercial 1.0.0 (Ayama's existing license)
  README.md               the product front door; the 6 dead ../../ links repaired
  build.bat               debug build      (PhyriadFG-shaped entry points)
  build-release.bat       release build
  testbench.ps1           build + tests — repaired to actually run them
  framework/              the vendored pillar closure (§4)
  action/ bench/ config/ core/ ipc/ learn/ observer/ policy/     the 8 submodules
  tools/                  ayama-agent, ayama-cli, ayama-bench, ayama-ui, installer
  scripts/                add-defender-exclusion.ps1
  docs/
    ARCHITECTURE.md       (PhyriadFG parity; to be written from the existing material)
    plans/                this triad + the 10 imported planning docs
    reports/             the 12 per-game empirical reports + raw_data/ (11 CSVs)
    FAQ.md, EMPIRICAL_TEST_PROTOCOL.md, …   imported from catalog/cpp/docs/apps/ayama/
```

## §6 — Phases and gates

| Phase | What | Gate |
|---|---|---|
| **0** | Skeleton + this triad | the three documents exist and are mutually linked |
| **1** | **Copy** (never move) `apps/ayama/*` → `projects/Ayama/` | file count matches the source |
| **2** | Vendor the §4 closure → `framework/`; re-point `PHYRIAD_ROOT`; exclude `render/vulkan/bench` | a from-scratch build is green |
| **3** | Repair the two defects §3 found: top-level `enable_testing()`, and the `learn_test.cpp:251` time-bomb | ctest **discovers 10 tests and runs them**; 10/10 pass |
| **4** | The front door: README link repair, LICENSE, build scripts, docs import | no dead relative link remains |
| **5** | **The decoupling proof** | Ayama configures + builds green with `catalog/cpp` renamed out of the way — the only honest evidence that vendoring worked |
| **6** | `git init` + initial commit | the project carries its own history (standing rule 2) |
| **7** | **Operator-gated:** remove `apps/ayama`; update the 7 container touchpoints | every risk `mitigated`/`accepted`; the operator's explicit go |

**Phase 1 copies rather than moves, and Phase 7 is a separate operator-gated step.** Until
Phase 7 runs, the migration is fully reversible by deleting `projects/Ayama` — nothing of the
operator's invested work is at stake (BOOT §3 universal rule 2: never delete invested work).
This is the primary mitigation for the Tier-2 data-loss trigger; see `DR1`.

## §7 — The 7 container touchpoints (Phase 7)

Verified to reference `apps/ayama` and to require an update when it disappears
(BOOT §3 universal rule 4 — touchpoints move together, or one becomes a lie):

1. `.clangd:18-25` — eight hardcoded absolute `-IG:/Phyriad/apps/ayama/<mod>/include` paths
   (generated by `scripts/gen_clangd.py` — regenerate, don't hand-edit)
2. `scripts/package_release.ps1:43` — `"-S", "apps/ayama",`
3. `.github/CODEOWNERS:53` — `/apps/ayama/ @Swately`
4. `.github/workflows/{ci-windows,release,release-ayama,release-please,ci.yml.disabled}.yml` —
   path filters and dist verification (already stale vs the retired root CMakeLists)
5. `catalog/cpp/INVENTORY.md:40-52+` — the `## apps/ayama/` section (gated by
   `scripts/check_inventory.py`)
6. `llm/scripts/extract_cpp_corpus.py:31,75` — corpus globs `apps/ayama/**/*.{hpp,cpp}`
7. `README.md:17` — the `apps/` table row naming ayama

Out of scope but noted: `catalog/cpp/CANON.md` names Ayama as the consumer of several pillars;
`protocols/quality/EMPIRICAL_TEST_PROTOCOL.md` cites Ayama as its source instance [V1];
`apps/myriarch` describes itself as Ayama's companion and mirrors its layout. Those are
*references to Ayama the project*, which remain true after the move — they are not path
dependencies. `bench/bench_*.cpp` mention Ayama in comments only.

## §8 — What this plan does NOT claim

- It does not claim the vendored closure in §4 is complete — only that it is grep-derived and
  that Phase 2's build is what settles it.
- It does not claim the 9 passing tests constitute adequate coverage. `config/` has no test
  source at all, and no test exercised the false green for an unknown number of weeks.
- It does not resolve the `CANON.md`-vs-code drift on `behavior`/`correlation`.
- It does not touch `apps/render_assistant` or `projects/PhyriadFG`.

<!-- Made with my soul - Swately <3 -->
