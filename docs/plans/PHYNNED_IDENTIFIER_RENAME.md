# Phynned identifier-level rename — record

**Date:** 2026-07-16 · **Scope:** the self-contained `projects/Phynned` tree
(formerly Ayama). The BRANDING rename landed earlier in commit `4b44fa3`; this
pass completes the **identifier-level** rename (namespaces, includes, targets,
options, class names, runtime literals, service/config identity). The vendored
substrate under `framework/` and the historical record were left untouched.

## Mapping (applied verbatim, case-sensitive)

| Domain | From | To |
|---|---|---|
| Namespace | `ayama` (`ayama::observer`, …) | `phynned` (`phynned::observer`, …) |
| Include dirs | `<mod>/include/ayama/` | `<mod>/include/phynned/` (git mv) |
| Include stmts | `#include <ayama/...>` | `#include <phynned/...>` |
| CMake lib/test targets | `ayama_<x>` (`ayama_observer`, `ayama_core_test`) | `phynned_<x>` |
| CMake options | `AYAMA_BUILD_TESTS/TOOLS/UI` | `PHYNNED_BUILD_TESTS/TOOLS/UI` |
| CMake vars | `AYAMA_RUNTIME_OUTPUT_DIRECTORY`, `AYAMA_INTERNAL_RUNTIME_DIRECTORY`, any other `AYAMA_*` (incl. `AYAMA_PRESENTMON_*`, `AYAMA_UI_SOURCES/SRCS`) | `PHYNNED_*` |
| Exe targets + output names | `ayama-agent/-cli/-bench/-ui/-service-register` | `phynned-agent/-cli/-bench/-ui/-service-register` |
| Tool dirs | `tools/ayama-{agent,cli,bench,ui}` | `tools/phynned-{agent,cli,bench,ui}` (git mv) |
| Dist folder | `<build>/ayama-dist` | `<build>/phynned-dist` |
| Resource files | `tools/phynned-ui/ayama-ui.rc` / `.manifest` (+ product strings) | `phynned-ui.rc` / `.manifest` (git mv) |
| Version header | `core/include/ayama/version.hpp`; macros `AYAMA_VERSION*`, `AYAMA_MAKE_VERSION` | `core/include/phynned/version.hpp`; `PHYNNED_VERSION*`, `PHYNNED_MAKE_VERSION` |
| Diag macro | `AYAMA_PHASE` | `PHYNNED_PHASE` |
| C++ class/type names | `\bAyama[A-Za-z0-9_]*` (AyamaClient, AyamaProtocol, AyamaAgentPublisher, AyamaAppState, AyamaLogicNode, AyamaTrayIcon, AyamaShmLayout, AyamaShmHeader, AyamaStateHeader, AyamaSnapshotMini, AyamaCmdKind, AyamaCommandSlot, AyamaKernel, AyamaPublisher, AyamaTrayMsgWnd, …) | `Phynned...` |
| Class-bearing files | `ipc/.../Ayama{Client,Protocol,AgentPublisher}.{hpp,cpp}`, `tools/phynned-ui/Ayama{AppState,LogicNode,TrayIcon}.hpp`, `tools/installer/ayama_service_register.cpp` | `Phynned...` / `phynned_service_register.cpp` (git mv — forced by the `#include`/`add_executable` token rename) |
| Log prefixes | `[Ayama]`, `[Ayama][X]` | `[Phynned]`, `[Phynned][X]` |
| SHM / kernel objects | `Local\AyamaAgent.v1`, `Local\AyamaAgentMutex.v1`, `/tmp/ayama-agent.lock` | `Local\PhynnedAgent.v1`, `Local\PhynnedAgentMutex.v1`, `/tmp/phynned-agent.lock` |
| Windows service | name `AyamaAgent`; display `Ayama Runtime Optimizer` | `PhynnedAgent`; `Phynned Runtime Optimizer` |
| Config dirs | `%LOCALAPPDATA%\Ayama`, `~/.config/ayama/` | `%LOCALAPPDATA%\Phynned`, `~/.config/phynned/` (clean break, no migration shim) |
| Release zip | `ayama-<version>-windows-x64.zip` | `phynned-<version>-windows-x64.zip` |
| File-header comments | `// apps/ayama/<path>` (+ stale `apps/ayama/` prose paths in WINDOWS_DISTRIBUTION.md) | project-relative (`// observer/src/ProcessObserver.cpp`, `CMakeLists.txt`, `scripts/...`) |

`phyriad` / `PHYRIAD_*` was **preserved everywhere** — it names the vendored
substrate family (e.g. `<phyriad/version.hpp>` from `framework/_meta`,
`phyriad_hal`, `-DPHYRIAD_BUILD_TESTS=ON`). `AYAMA` and `PHYRIAD` share no
substring, so the case-sensitive replace could not collide.

## Exclusions (left byte-untouched)

- `framework/**` — vendored substrate (kept identical to catalog for drift-diffing).
- `docs/plans/AYAMA_SEPARATION_*.md` and `docs/plans/history/**` — historical record.
- `docs/reports/**` — evidence archive (incl. `raw_data/ayama_on.csv`, kept by name).
- `.git`, `build/`, `build-release/`.

## Allowed residual "Ayama" mentions after the pass

1. **README.md** rename note (former name + the `%LOCALAPPDATA%\Ayama`
   no-migration callout) and **docs/ARCHITECTURE.md** link to the excluded
   `plans/AYAMA_SEPARATION_MASTER_PLAN.md` — both allowed by spec.
2. **Deviation (5 sites):** in-code / in-doc **citations to historical plan
   docs by their preserved filenames** were kept verbatim, because those files
   live under the excluded `docs/plans/**` and keep their `AYAMA_*` names —
   renaming the citation would dangle the reference (a regression). Sites:
   - `core/include/phynned/core/AgentRuntime.hpp:41` — `AYAMA_IMPLEMENTATION_STRATEGIES.md §3.2`
   - `core/include/phynned/core/SelfMonitor.hpp:8` — `AYAMA_MASTER_PLAN.md §0.6 / §10.1`
   - `policy/include/phynned/policy/AutoPolicySelector.hpp:4` — `AYAMA_MASTER_PLAN`
   - `tools/phynned-ui/main.cpp:421` — `AYAMA_MASTER_PLAN.md changelog`
   - `docs/EMPIRICAL_TEST_PROTOCOL.md:13,515` — `AYAMA_MASTER_PLAN.md §8.6 / §5.4`

## Verification

- **Fresh build gate:** `build/` deleted, `testbench.ps1` re-run from clean
  (re-fetched GLFW 3.4 + ImGui v1.91.5-docking).
- **Result (elevated / admin shell):** 48/49 tests pass. The ONLY failure is
  `agent_idle_budget_test` — the KNOWN pre-existing defect (documented in
  `docs/ARCHITECTURE.md`): under admin the ETW session really starts and its
  teardown makes `stop()` take ~5.07 s vs the 2 s bound. Not fixed / not masked
  per instruction (49/49 unelevated).
- **Dist output present:** `build/phynned-dist/phynned-ui.exe` +
  `build/phynned-dist/runtime/phynned-{agent,cli,bench,service-register}.exe`.
- **Grep-proof:** `grep -ri ayama` over the tree (minus exclusions) returns only
  the allowed residuals enumerated above.

<!-- Made with my soul - Swately <3 -->
