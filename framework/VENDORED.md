# framework/ — vendored Phyriad pillar snapshot

**Provenance:** copied from `G:\phyriad\catalog\cpp` on **2026-07-16** during the Ayama
separation (see `docs/plans/AYAMA_SEPARATION_MASTER_PLAN.md`).

**This is Phynned's own base now.** It is deliberately a fork: modify these sources freely for
the project's needs — that independence is the point of the separation (the PhyriadFG model). The
cost, accepted by the operator (risk `SR1`): fixes landing in `catalog/cpp` after 2026-07-16 do
NOT reach this copy on their own, and nobody is watching the drift.

**Operator directive 2026-07-17 — no catalog-parity discipline, no Phyriad code philosophy.**
Phynned's code (this vendored `framework/` included) is explicitly NOT bound to Phyriad's
substrate ENGINEERING philosophy — the `DOGMA_SPECIFICATIONS` D-1..D-21 (lock-free mandate, HAL
discipline, zero-alloc hot paths, zero-overhead) do NOT govern here. Reshape these pillars,
restructure them, or pick the simplest correct primitive (a `std::mutex` over a forced lock-free
ring) as the project needs. There is **no obligation to keep this tree diffable against
`catalog/cpp`** — the byte-parity framing below is retired; defects are simply fixed in Phynned's
copy. (This releases the CODE philosophy only; the SESSION's conduct — verify-before-claim,
calibrated reporting, the L3 safety gate, plan-tier — still binds, per BOOT §1, and so does the
authorship signature.)

**The 17 vendored units** (the dependency closure of Ayama's sources, settled by the linker,
not by grep alone):

| Unit | Why it is here |
|---|---|
| `hal` | directly included (17 sites); the leaf every pillar rests on |
| `schema` | directly included (13 sites) |
| `topology` | directly included (8) — affinity/priority free functions, the core mechanism |
| `tuning` | directly included (5) |
| `process` | directly included (4) |
| `stigmergy` | directly included (2) — the Classifier |
| `etw` | directly included (1) — the read-only game detector |
| `ipc` | directly included (1) — agent↔ui SHM |
| `node` | directly included (1) + via `ui` |
| `ui` | directly included (6) — ayama-ui's application layer |
| `render` | linked by `ui`; **`vulkan/bench` excluded** (275 MB of FG bench artifacts, no Ayama consumer) |
| `graph` | hosts `phyriad/api/*` headers (4 Ayama include sites live in `graph/include/phyriad/api/`) |
| `scheduler` | dependency of `graph`, `runtime`, `profile` |
| `transport` | dependency of `node`, `graph`, `stigmergy` |
| `runtime` | hard-linked by `phyriad_ui` |
| `profile` | dependency of `runtime` |
| `_meta` | `<phyriad/version.hpp>` |

**Trimmed at separation** (in `catalog/cpp`'s Ayama build, absent here): `orchestration`,
`behavior`, `correlation` — zero `#include` and zero `target_link_libraries` sites in Ayama's
sources as of 2026-07-16. Note `catalog/cpp/CANON.md` names Ayama as a consumer of `behavior`
(PressureScore) and `correlation`; the code contradicts that — the drift is recorded in the
master plan §4, not resolved here. Re-vendor from `catalog/cpp` if a future feature needs one.

Local `build/`, `_deps/`, `.cache/` directories were excluded from the copy.

**Known inherited defects** (faithfully copied, NOT migration-introduced): the standalone-as-
CMake-root fallback guards in `render/composite/CMakeLists.txt:32,35` (`../../../render`,
`../../../schema` — wrong arithmetic, resolve to nonexistent paths) and
`render/opengl3/CMakeLists.txt:15` (`../..` targets `framework/`, which has no CMakeLists). Both
are **unreachable in Phynned's build** (the parent defines the targets before these guards are
evaluated) and equally broken in the `catalog/cpp` original. **No longer left as-is for parity**
(directive 2026-07-17): fix freely in Phynned's copy when convenient — the earlier "keep it
diffable" reason is retired. Also on the free-fix list: the ETW provider-GUID defects in
`etw/SessionManager.hpp` (the mass-router's S0 repair — no catalog coordination needed).

**Note:** the identifier rename (2026-07-16, `ayama::`→`phynned::`) deliberately EXCLUDED
`framework/**` so the tree stayed diffable against catalog at that moment; with the parity
discipline now retired, the vendored pillars may be renamed/reshaped into Phynned's own idiom
whenever a change touches them — there is no reason to preserve the `phyriad::` names beyond
inertia, though a bulk rename is its own scoped task, not required.

<!-- Made with my soul - Swately <3 -->
