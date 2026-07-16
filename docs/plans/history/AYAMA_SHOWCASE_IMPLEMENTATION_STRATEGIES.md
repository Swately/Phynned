> **📦 Archived planning document — historical.**
> From Phyriad's pre-rebrand era (the project was codenamed *gamma* / *gma*).
> Published for transparency about how the project was actually built — **not**
> a current specification. Version numbers and performance claims here
> (including any "vs Folly" / "production-ready" framing) reflect the project's
> state at the time of writing and may since have been revised or **retracted**;
> the project has reset to `v0.1.0-experimental`. For current status see the
> repo `README.md` and `docs/planning/`. For benchmark-claim validation status
> see `docs/planning/TEST_INVENTORY.md`.

# Ayama Showcase — Implementation Strategies
## Patrones técnicos para máximo rendimiento durante la transición a release público

**Version:** 1.0 — Mayo 2026
**Foundation:** Gamma Framework 1.1.0 + Ayama Phase B.2
**Companion doc:** [`AYAMA_SHOWCASE_MASTER_PLAN.md`](AYAMA_SHOWCASE_MASTER_PLAN.md)
**Audience:** implementadores de cada Tier/Feature del Showcase Master Plan.

Este documento es la **caja de herramientas técnica** para la fase de showcase.
Cuando un item del plan está a punto de implementarse, primero se consulta acá
si hay un patrón aplicable.

---

## Tabla de contenidos

- [§0 — Principios de máximo rendimiento](#0--principios-de-máximo-rendimiento)
- [§1 — Tier 0: patterns para repo + CI + distribución](#1--tier-0-patterns-para-repo--ci--distribución)
- [§2 — Tier 1: patterns para charts y evidencia](#2--tier-1-patterns-para-charts-y-evidencia)
- [§3 — Tier 2: patterns para framework standalone](#3--tier-2-patterns-para-framework-standalone)
- [§4 — Feature A: patterns para Compute Graph](#4--feature-a-patterns-para-compute-graph)
- [§5 — Feature B: patterns para Topology Map](#5--feature-b-patterns-para-topology-map)
- [§6 — Cross-cutting: logging, errors, threading invariants](#6--cross-cutting-logging-errors-threading-invariants)
- [§7 — Validación y benchmarks de regresión](#7--validación-y-benchmarks-de-regresión)

---

## §0 — Principios de máximo rendimiento

### §0.1 — Las 10 reglas inquebrantables

Heredadas y extendidas del doc core `AYAMA_IMPLEMENTATION_STRATEGIES.md`:

1. **Zero allocation en steady state.** Todo buffer/struct del hot path se construye en `start()`. Cero `new`, cero `malloc`, cero `std::vector::push_back` que crezca, cero `std::string` mutación post-init.
2. **POD types para cross-boundary data.** IPC, SHM, persistence — todo es `std::is_trivially_copyable_v` + `std::is_standard_layout_v`.
3. **Cache-line alignment para hot structures.** `alignas(64)` en cualquier struct que se escribe/lee con frecuencia desde múltiples threads.
4. **Lock-free atomics para cross-thread.** Mutexes solo para paths cold (init, config reload). Producer-consumer = ring buffer atómico.
5. **Static polymorphism > virtual dispatch.** Concepts C++23 + CRTP. `virtual` solo donde se requiere ABI estable cross-DLL (pocas excepciones).
6. **constexpr / consteval donde aplique.** Compile-time computation siempre preferible a runtime equivalent.
7. **noexcept everywhere.** Better codegen (no exception tables), garantía explícita de no-throw, requirement de Gamma.
8. **`[[nodiscard]]` en errores y returns relevantes.** El compiler atrapa bugs por noi-uso.
9. **Branchless donde posible y legible.** `cmov`/`andn`/`setcc` vs branch-mispredicted hot paths.
10. **Profile-guided over speculative.** Medir con `gma::hal::rdtsc()` antes de optimizar. "Avoid premature optimization, embrace measured optimization".

### §0.2 — Resource budgets (no negociables)

Heredados del §10 del Master Plan core, válidos durante TODOS los items del showcase:

| Recurso | Budget idle | Budget active | Budget bench |
|---|---|---|---|
| CPU sustained (agent) | `<` 0.5% | `<` 3% | `<` 5% |
| RSS (agent) | `<` 20 MB | `<` 50 MB | `<` 50 MB |
| Disk I/O (agent) | `<` 1 KB/s | `<` 10 KB/s | `<` 100 KB/s |
| UI draw cost | n/a | `<` 16 ms/frame | `<` 16 ms/frame |
| Per-tick overhead añadido | `<` 100 µs | `<` 100 µs | `<` 100 µs |

Cada feature nueva mide su delta contra estos antes de mergear. El `SelfMonitor` reporta violaciones en runtime.

### §0.3 — Tooling para medir performance

- **`gma::hal::rdtsc()`**: ~10 cycles, monotónico, no requiere syscall. Para hot path timing.
- **`QueryPerformanceCounter`**: ~80 cycles, monotónico. Para wall-clock cross-platform.
- **`SetThreadAffinityMask + spinwait` + rdtsc**: para micro-benchmarks bare-metal (Phase B.2 hot thread heuristic usa esto).
- **VTune / Tracy / perfomance trace**: para profiling externo. No requerido pero útil.
- **AYAMA_PHASE markers** ya presentes en `AgentRuntime.cpp`: dan visibilidad básica gratis.

### §0.4 — Lenguaje y std lib

- **C++23**: explotar `std::expected`, concepts, `[[nodiscard]]`, `consteval`.
- **MinGW-w64 + GCC 15**: toolchain por defecto. MSVC también soportado en CI.
- **NO STL containers en hot path**: `std::vector`/`std::map`/`std::unordered_map` allocan. Reemplazos:
  - `std::array<T, N>` para fixed-size
  - Custom hash tables (e.g. `pid_hash_` en MetricsCollector)
  - `gma::ipc::Ring<T, Capacity>` (FR-12)
- **Headers C-compatibility**: para Gamma APIs públicas que vayan a usarse desde C, considerar wrappers.

---

## §1 — Tier 0: patterns para repo + CI + distribución

### §1.1 — `.gitignore` comprehensivo

```gitignore
# Build artifacts
build/
build_*/
*.obj
*.o
*.exe
*.dll
*.lib
*.a
*.pdb
*.ilk
*.exp

# CMake
CMakeCache.txt
CMakeFiles/
cmake_install.cmake
compile_commands.json
*.ninja_log
.ninja_*

# Logs from agent + bench runs
*.log
agent_*.log
audit.bin

# Editor / IDE
.vscode/settings.json
.vs/
*.user
.idea/
*.swp

# OS
.DS_Store
Thumbs.db
desktop.ini

# Project-specific clutter
_legacy/
*.zip          # backups go elsewhere
PresentMon*.exe
PresentMon*.log

# Coverage / profiling
*.gcda
*.gcno
*.profraw
*.profdata

# Doxygen output (generated)
docs/gamma/html/
docs/gamma/xml/
docs/gamma/latex/
```

Rationale: minimizar push de bloat. CI regenera todo, no necesita persistir.

### §1.2 — GitHub Actions matrix óptima

```yaml
# .github/workflows/ci.yml
name: CI

on: [push, pull_request]

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        os: [windows-2022]
        toolchain: [msvc, mingw]
        config: [Release]   # Debug solo si tests fallan en CI específicamente
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - name: Cache CMake build
        uses: actions/cache@v4
        with:
          path: build/
          key: ${{ matrix.toolchain }}-${{ hashFiles('**/CMakeLists.txt') }}
      - name: Configure
        run: cmake -S . -B build -G Ninja
      - name: Build
        run: cmake --build build --config Release --parallel
      - name: Test
        run: ctest --test-dir build --output-on-failure
```

Optimizaciones:
- **Ninja**: 30-50% faster que MSBuild para incremental.
- **`--parallel`**: usa todos los cores del runner (2-4 en GitHub free tier).
- **Cache de build dir**: incremental builds en PRs subsecuentes.
- **`fail-fast: false`**: ambos toolchains se reportan aunque uno falle.

Target: full build de cero en `<` 5 min, incremental con cache en `<` 90 s.

### §1.3 — Release packaging strategy

Script `scripts/package_release.ps1`:

```powershell
# Recipe high-perf release zip
param([string]$Version = "0.5.0")

$work = "release-staging"
$out = "ayama-$Version-windows-x64.zip"

# 1. Build release config con LTO + strip
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON `
      -DCMAKE_EXE_LINKER_FLAGS="-s"   # strip symbols
cmake --build build-release --parallel

# 2. Copia solo los binarios esenciales
New-Item -ItemType Directory -Path $work -Force
Copy-Item build-release/ayama/tools/ayama-agent/ayama-agent.exe $work/
Copy-Item build-release/ayama/tools/ayama-ui/ayama-ui.exe $work/
Copy-Item build-release/ayama/tools/ayama-cli/ayama-cli.exe $work/

# 3. Bundles docs reducidas (no internas)
Copy-Item README.md $work/
Copy-Item LICENSE $work/
Copy-Item FAQ.md $work/
Copy-Item docs/REPRODUCING_BENCHMARKS.md $work/

# 4. PresentMon va en zip separado (download script lo trae)
Copy-Item scripts/install.ps1 $work/

# 5. Compresión óptima
Compress-Archive -Path "$work/*" -DestinationPath $out -CompressionLevel Optimal

# 6. Verifica tamaño
$size = (Get-Item $out).Length / 1MB
if ($size -gt 20) {
    Write-Warning "Release zip is $size MB (target < 20 MB)"
}

Remove-Item -Recurse $work
```

Target: `< 20 MB` final. Stripped binaries son típicamente 5-10 MB cada uno.

### §1.4 — Install script idempotente

```powershell
# install.ps1 — runs from extracted release zip
param([switch]$Force)

# 1. Detect admin (PresentMon + Ayama agent requieren admin)
if (-not ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "Run as Administrator (right-click → Run as administrator)"
    exit 2
}

# 2. Install location
$installDir = "$env:LOCALAPPDATA\Ayama"
if ((Test-Path $installDir) -and -not $Force) {
    Write-Host "Already installed at $installDir. Use -Force to reinstall."
    exit 0
}

# 3. Copy binaries (parallel since they're small files)
New-Item -ItemType Directory -Path $installDir -Force | Out-Null
Get-ChildItem "$PSScriptRoot\*.exe" | ForEach-Object -Parallel {
    Copy-Item $_.FullName $using:installDir -Force
} -ThrottleLimit 4

# 4. Download PresentMon if not bundled (rare; usually CMake fetches it)
$pmPath = "$installDir\PresentMon.exe"
if (-not (Test-Path $pmPath)) {
    $pmUrl = "https://github.com/GameTechDev/PresentMon/releases/download/v2.4.1/PresentMon-2.4.1-x64.exe"
    Invoke-WebRequest -Uri $pmUrl -OutFile $pmPath -UseBasicParsing
}

# 5. Add to PATH (HKCU, no admin needed for env)
$path = [Environment]::GetEnvironmentVariable("Path", "User")
if ($path -notlike "*$installDir*") {
    [Environment]::SetEnvironmentVariable("Path", "$path;$installDir", "User")
}

# 6. Start menu shortcut
$start = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Ayama.lnk"
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut($start)
$lnk.TargetPath = "$installDir\ayama-ui.exe"
$lnk.Save()

Write-Host "Installed to $installDir"
Write-Host "Launch: ayama-ui (from any terminal) or Start Menu → Ayama"
```

Target: idempotente (run 2x = mismo resultado), `<` 60 s en conexión 50 Mbps, requiere admin.

---

## §2 — Tier 1: patterns para charts y evidencia

### §2.1 — Hero plot generation

Script `docs/figures/generate_halo_curve.py`:

```python
#!/usr/bin/env python3
"""Generate the Halo MCC empirical curve hero plot."""
import matplotlib
matplotlib.use('Agg')  # no display needed
import matplotlib.pyplot as plt
import numpy as np

# Data from reports — verifiable from EMPIRICAL_EVIDENCE_SUMMARY.md
games = [
    ("Halo CE Anniversary", 2001, 98.0,  0.95),
    ("Halo 2 MCC",          2004, 56.0,  0.90),
    ("Halo 3 MCC",          2007, 19.3,  0.70),
    ("Halo Reach MCC",      2010,  1.8,  0.45),
    ("Halo 4 MCC",          2012,  0.0,  0.30),
]
years   = [g[1] for g in games]
fps_d   = [g[2] for g in games]
hot_th  = [g[3] * 100 for g in games]
names   = [g[0] for g in games]

fig, ax1 = plt.subplots(figsize=(10, 6), dpi=100)
ax2 = ax1.twinx()

bars = ax1.bar(years, fps_d, width=2.0, color='steelblue',
               alpha=0.75, label='Δ Avg FPS (%)')
ax1.set_xlabel('Game engine year (original release)')
ax1.set_ylabel('Δ Avg FPS with Ayama Rule 1 (%)', color='steelblue')

line = ax2.plot(years, hot_th, color='darkorange',
                marker='o', linewidth=2,
                label='Hot thread CPU share (%)')
ax2.set_ylabel('Hot thread CPU share (%)', color='darkorange')

# Annotate Halo CE as the record
ax1.annotate('NEW RECORD', xy=(2001, 98), xytext=(2002, 105),
             fontsize=10, fontweight='bold', color='darkred',
             arrowprops=dict(arrowstyle='->', color='darkred'))

# Title + grid
plt.title('Ayama benefit decreases monotonically as engine\'s thread '
          'concentration drops\n'
          '(AMD Ryzen 9 7950X3D + RTX 4090, 5-run A/B/A/B/A per game)')
ax1.grid(True, alpha=0.3)
fig.tight_layout()
plt.savefig('docs/figures/halo-mcc-curve.png', dpi=150, bbox_inches='tight')
plt.savefig('docs/figures/halo-mcc-curve.svg', bbox_inches='tight')
```

Patterns:
- **`matplotlib.use('Agg')`** sin display: ~5x más rápido que con backend interactivo.
- **PNG + SVG dual output**: PNG para README rapid load, SVG para zoom-in en GitHub Pages.
- **dpi=150**: balance entre archivo pequeño (`<` 100 KB) y crisp en displays 4K.
- **Datos hardcoded en script**: no parse de CSVs at gen time. Cada vez que cambia evidence, edit este script (~30 LOC).

### §2.2 — Comparison harness pattern

Para Feature C (deferred): la lógica de "toggle other tool, run bench" si se implementa, debe vivir en `ayama-cli bench compare`:

```cpp
// ayama-cli/bench_compare_main.cpp
struct CompareScenario {
    const char* name;
    void (*setup)();    // turn on tool
    void (*teardown)(); // turn off tool
};

CompareScenario kScenarios[] = {
    {"baseline",        []{}, []{}},
    {"win11_game_mode", enable_game_mode, disable_game_mode},
    {"process_lasso",   enable_pl_probalance, disable_pl_probalance},
    {"ayama",           start_ayama_agent, stop_ayama_agent},
};
```

Pattern: zero-allocation, static array. Cada scenario se ejecuta full A/B/A/B/A, resultados acumulados en una matriz.

### §2.3 — Reports cleanup checklist

Para cada report en `docs/ayama/reports/*.md`:

- [ ] Header con `Status: VALID / SUPERSEDED / INVALID` claro
- [ ] Date, hardware, software versions presentes
- [ ] Tabla de aggregate numbers con stddev
- [ ] Raw per-run data preserved
- [ ] Reproducibility section ejecutable
- [ ] No referencias a paths internos del autor (e.g. `F:/...`)

Patrón de search&replace:
```powershell
Get-ChildItem docs/ayama/reports/*.md | ForEach-Object {
    (Get-Content $_) -replace 'F:/gma_1\.0\.0/', '<repo-root>/' |
        Set-Content $_
}
```

---

## §3 — Tier 2: patterns para framework standalone

### §3.1 — CMake split clean

`pillars/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)

if(GAMMA_STANDALONE)
    project(Gamma VERSION 1.1.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Each pillar is a target
add_subdirectory(_meta)      # version macros
add_subdirectory(schema)
add_subdirectory(hal)
add_subdirectory(topology)
add_subdirectory(process)
add_subdirectory(tuning)
add_subdirectory(ipc)
add_subdirectory(etw)
add_subdirectory(graph)      # NEW for Feature A
# ... etc

# Aggregate target for downstream find_package
add_library(Gamma INTERFACE)
target_link_libraries(Gamma INTERFACE
    gamma::schema
    gamma::hal
    gamma::topology
    gamma::process
    gamma::tuning
    gamma::ipc
    gamma::etw
    gamma::graph
)

# Install rules (only when standalone or explicitly requested)
if(GAMMA_STANDALONE OR GAMMA_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS Gamma EXPORT GammaTargets)
    install(EXPORT GammaTargets
        FILE GammaTargets.cmake
        NAMESPACE Gamma::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Gamma)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/GammaConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
endif()
```

Cada pillar:
```cmake
# pillars/topology/CMakeLists.txt
add_library(gamma_topology STATIC src/HardwareTopology.cpp)
add_library(gamma::topology ALIAS gamma_topology)

target_include_directories(gamma_topology
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(gamma_topology PUBLIC cxx_std_23)
```

Patrón: cada pillar es STATIC library para link-time inlining máximo. INTERFACE library agregada para downstream conveniencia.

### §3.2 — Example pattern: process_monitor

`pillars/examples/process_monitor/main.cpp` (target: `< 50 LOC`):

```cpp
// Standalone Gamma demo: list top-10 processes by CPU% (no Ayama).
//
// Builds as: cmake -S . -B build && cmake --build build && ./build/process_monitor

#include <gamma/process/ProcessMetricsSnapshot.hpp>
#include <algorithm>
#include <cstdio>
#include <thread>
#include <chrono>

int main() {
    auto snap = gma::proc::ProcessMetricsSnapshot::create();
    if (!snap) { std::fprintf(stderr, "create failed\n"); return 1; }

    // First capture sets baseline times.
    (void)snap->capture();
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Second capture gives meaningful delta.
    if (auto r = snap->capture(); !r) { return 2; }

    // Sort by total CPU time descending.
    std::vector<gma::proc::ProcessMetrics> all(
        snap->data(), snap->data() + snap->process_count());
    std::sort(all.begin(), all.end(),
        [](auto& a, auto& b) noexcept {
            return (a.kernel_time_100ns + a.user_time_100ns)
                 > (b.kernel_time_100ns + b.user_time_100ns);
        });

    std::printf("%-32s %10s %12s\n", "Process", "Threads", "CPU 100ns");
    for (size_t i = 0; i < std::min<size_t>(10, all.size()); ++i) {
        std::printf("%-32u %10u %12llu\n",
                    all[i].pid, all[i].thread_count,
                    static_cast<unsigned long long>(
                        all[i].kernel_time_100ns + all[i].user_time_100ns));
    }
    return 0;
}
```

Patterns:
- Demo standalone (no Ayama includes)
- `std::vector` permitido en demos (NO en hot path real)
- API de Gamma usable sin special setup (just `create()`)
- Output útil inmediato (top 10 procesos por CPU)

### §3.3 — Doxygen configuration óptima

`docs/gamma/Doxyfile`:

```
PROJECT_NAME           = "Gamma Framework"
PROJECT_NUMBER         = 1.1.0
OUTPUT_DIRECTORY       = docs/gamma/build
INPUT                  = pillars/topology/include \
                         pillars/process/include \
                         pillars/tuning/include \
                         pillars/ipc/include \
                         pillars/etw/include \
                         pillars/hal/include \
                         pillars/schema/include \
                         pillars/graph/include
RECURSIVE              = YES
EXTRACT_ALL            = NO     # respect access modifiers
EXTRACT_PRIVATE        = NO
EXTRACT_STATIC         = NO
GENERATE_HTML          = YES
GENERATE_LATEX         = NO     # PDF takes forever, skip
HAVE_DOT               = YES    # call graphs
DOT_GRAPH_MAX_NODES    = 50     # bounded for speed
INTERNAL_DOCS          = NO
QUIET                  = YES
WARN_NO_PARAMDOC       = YES
```

Optimizaciones:
- **`EXTRACT_PRIVATE = NO`**: solo APIs públicas, output más pequeño
- **`GENERATE_LATEX = NO`**: PDF gen toma 30+ s
- **`DOT_GRAPH_MAX_NODES = 50`**: previene call graphs explosivos
- **`INPUT` específico**: solo headers, no source. ~2x más rápido

Target: full Doxygen build en `<` 30 s.

---

## §4 — Feature A: patterns para Compute Graph

### §4.1 — API design con concepts C++23

```cpp
// pillars/graph/include/gamma/graph/Concepts.hpp
#pragma once
#include <concepts>

namespace gma::graph {

template <typename T>
concept ExecutableNode = requires(T t) {
    { t.execute() } noexcept -> std::same_as<void>;
    { T::name() } noexcept -> std::convertible_to<const char*>;
};

template <typename T>
concept TimedNode = ExecutableNode<T> && requires(T t) {
    { t.last_tsc_cycles() } noexcept -> std::convertible_to<uint64_t>;
};

} // namespace gma::graph
```

Cada node es un struct/class que satisfice `ExecutableNode`:

```cpp
struct MetricsSampleNode {
    static const char* name() noexcept { return "metrics_sample"; }
    void execute() noexcept { /* ... */ }
};
static_assert(gma::graph::ExecutableNode<MetricsSampleNode>);
```

Patrón: **zero virtual dispatch**. El graph runtime usa template instantiation por node type. Compile-time todo.

### §4.2 — Graph storage layout

```cpp
// pillars/graph/include/gamma/graph/Graph.hpp
namespace gma::graph {

constexpr uint32_t kMaxNodes = 64;
constexpr uint32_t kMaxEdges = 256;
constexpr uint32_t kMaxParallelCohorts = 16;

struct alignas(64) NodeSlot {
    void (*execute_fn)(void* node_ptr) noexcept;   // type-erased dispatch
    void* node_ptr;                                 // stable pointer to node
    const char* name;
    uint8_t  depth;          // topological depth
    uint8_t  cohort_idx;     // which parallel cohort
    uint16_t _pad;
    // Per-tick timing
    uint64_t last_tsc_start;
    uint64_t last_tsc_end;
    uint64_t cumulative_cycles;
    uint32_t invocation_count;
};

struct Graph {
    alignas(64) std::array<NodeSlot, kMaxNodes> nodes;
    uint32_t n_nodes{0};

    // Edges as adjacency lists encoded as arrays of (from, to) pairs.
    std::array<std::pair<uint8_t,uint8_t>, kMaxEdges> edges;
    uint32_t n_edges{0};

    // Topological execution order (set by compile()).
    std::array<uint8_t, kMaxNodes> exec_order;

    // Cohort = set of nodes that can run in parallel (same depth, no edges among them).
    std::array<std::pair<uint8_t,uint8_t>, kMaxParallelCohorts> cohort_ranges;  // [start, end) into exec_order
    uint32_t n_cohorts{0};

    bool compiled{false};
};

} // namespace gma::graph
```

Patterns:
- **`alignas(64)` en NodeSlot**: cada node en su propia cache line (write contention zero entre nodes en threads diferentes).
- **`uint8_t` indices**: kMaxNodes=64 cabe en uint8_t. Compact storage.
- **`void* node_ptr`**: type-erased — el graph no necesita conocer el tipo concreto, solo el `execute_fn`.
- **`function pointer` static**: cero overhead vs virtual (no vtable lookup).

### §4.3 — Type-erased dispatch sin allocation

```cpp
template <typename NodeT>
requires gma::graph::ExecutableNode<NodeT>
void execute_thunk(void* p) noexcept {
    static_cast<NodeT*>(p)->execute();
}

template <typename NodeT>
NodeHandle Graph::add_node(NodeT& node, const char* name) noexcept {
    if (n_nodes >= kMaxNodes) return {};
    auto& slot = nodes[n_nodes];
    slot.execute_fn = &execute_thunk<NodeT>;
    slot.node_ptr   = static_cast<void*>(&node);
    slot.name       = name;
    return NodeHandle{n_nodes++};
}
```

Patrón: el `execute_thunk<T>` se genera por el compiler para cada `NodeT`, es una función simple que cast + call. Zero overhead vs llamada directa cuando inlines (probable en `-O3 -flto`).

### §4.4 — Topological sort + cohort detection

```cpp
bool Graph::compile() noexcept {
    if (compiled) return true;

    // 1. Compute in-degrees via static array (no heap)
    std::array<uint8_t, kMaxNodes> in_degree{};
    for (uint32_t i = 0; i < n_edges; ++i) {
        ++in_degree[edges[i].second];
    }

    // 2. Kahn's algorithm via static queue
    std::array<uint8_t, kMaxNodes> queue;
    uint32_t qhead = 0, qtail = 0;

    for (uint32_t i = 0; i < n_nodes; ++i) {
        if (in_degree[i] == 0) queue[qtail++] = i;
    }

    uint32_t exec_idx = 0;
    uint8_t current_depth = 0;
    uint32_t cohort_start = 0;

    while (qhead < qtail) {
        const uint32_t cohort_size = qtail - qhead;
        for (uint32_t k = 0; k < cohort_size; ++k) {
            const uint8_t u = queue[qhead++];
            nodes[u].depth = current_depth;
            nodes[u].cohort_idx = n_cohorts;
            exec_order[exec_idx++] = u;

            // Visit successors
            for (uint32_t e = 0; e < n_edges; ++e) {
                if (edges[e].first == u) {
                    const uint8_t v = edges[e].second;
                    if (--in_degree[v] == 0) queue[qtail++] = v;
                }
            }
        }
        // Mark end of cohort
        cohort_ranges[n_cohorts++] = {cohort_start, exec_idx};
        cohort_start = exec_idx;
        ++current_depth;
    }

    if (exec_idx != n_nodes) return false;  // cycle detected
    compiled = true;
    return true;
}
```

Patrón: `compile()` corre **una sola vez** en `start()`, no en runtime. Compute es O(V+E) = O(kMaxNodes + kMaxEdges) ≈ 320 operaciones. ~1 µs.

### §4.5 — Execute con paralelismo opcional

```cpp
void Graph::run() noexcept {
    for (uint32_t c = 0; c < n_cohorts; ++c) {
        const auto [start, end] = cohort_ranges[c];

        if (end - start == 1) {
            // Solo un node — ejecuta inline
            execute_node(exec_order[start]);
        } else {
            // Cohort paralelo — dispatch a thread pool
            execute_parallel_cohort(start, end);
        }
    }
}

void Graph::execute_node(uint8_t idx) noexcept {
    auto& slot = nodes[idx];
    slot.last_tsc_start = gma::hal::rdtsc();
    slot.execute_fn(slot.node_ptr);
    slot.last_tsc_end = gma::hal::rdtsc();
    slot.cumulative_cycles += (slot.last_tsc_end - slot.last_tsc_start);
    ++slot.invocation_count;
}
```

`execute_parallel_cohort` usa `gma::hal::ThreadPool` (pillar a expandir si no existe) con barrier. Cada thread agarra un node del cohort hasta vaciarse.

Patterns:
- **Inline path para cohort de 1**: evita overhead de pool dispatch cuando no hay paralelismo.
- **TSC bracketing**: ~20 cycles overhead total por node. En budget.
- **Cumulative cycles**: para histograma de costo, sin allocations.

### §4.6 — UI Pipeline panel rendering

```cpp
// dashboard "Pipeline" tab
void draw_pipeline_panel(const Graph& g) noexcept {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    constexpr float kNodeW = 140.0f;
    constexpr float kNodeH = 50.0f;
    constexpr float kSpaceX = 30.0f;
    constexpr float kSpaceY = 20.0f;

    for (uint32_t i = 0; i < g.n_nodes; ++i) {
        const auto& slot = g.nodes[i];
        const float x = origin.x + slot.depth * (kNodeW + kSpaceX);
        // Y position: based on rank within cohort
        const float y = origin.y + cohort_y_offset(g, i) * (kNodeH + kSpaceY);

        // Color by timing
        const uint64_t cycles = slot.last_tsc_end - slot.last_tsc_start;
        const double ms = cycles / 4.2e6;  // approx for 4.2 GHz TSC
        const ImU32 color = (ms < 1.0)  ? IM_COL32(60, 200, 100, 255)
                          : (ms < 5.0)  ? IM_COL32(220, 180, 40, 255)
                                        : IM_COL32(220, 80, 60, 255);

        dl->AddRectFilled({x, y}, {x + kNodeW, y + kNodeH}, color, 6.0f);
        dl->AddText({x + 8, y + 8}, IM_COL32_WHITE, slot.name);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f ms", ms);
        dl->AddText({x + 8, y + 28}, IM_COL32_WHITE, buf);
    }

    // Edges
    for (uint32_t e = 0; e < g.n_edges; ++e) {
        // ... draw bezier from end of slot[from] to start of slot[to]
    }
}
```

Patterns:
- **Direct `ImDrawList` primitives**: no overhead de full ImGui widgets.
- **Cycle-based timing**: convertir a ms solo para display, evitar float math en hot path.
- **Constant layout**: `kNodeW`, `kNodeH` constexpr.
- **Color lookup branchless**: ternary chain compila a `cmov`.

Target: `< 0.5 ms / frame` para render del pipeline panel completo.

---

## §5 — Feature B: patterns para Topology Map

### §5.1 — Per-core CPU% sampling

Extender `gma::proc::ProcessMetricsSnapshot` para exponer per-core data. NtQuerySystemInformation con `SystemProcessorPerformanceInformation` (class 8) da idle/kernel/user time por CPU:

```cpp
// pillars/process/include/gamma/process/CpuLoadSnapshot.hpp
namespace gma::proc {

struct alignas(8) CpuLoadEntry {
    uint64_t idle_time_100ns;
    uint64_t kernel_time_100ns;
    uint64_t user_time_100ns;
};

class CpuLoadSnapshot {
public:
    [[nodiscard]] static std::expected<CpuLoadSnapshot, gma::Error> create() noexcept;
    [[nodiscard]] std::expected<void, gma::Error> capture() noexcept;

    /// Compute per-core utilization% as (kernel_delta + user_delta) / wall_delta.
    /// Requires two consecutive captures. Out array size = topo.logical_core_count().
    void compute_utilization(float* out_util_pct, uint32_t max_cores) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gma::proc
```

Implementación calls NtQSI con class 8, ~50 µs overhead. Mucho más barato que enumerar todos los procesos.

### §5.2 — Per-core history ring

```cpp
// In TopologyMap panel state
constexpr uint32_t kMaxCores = 64;
constexpr uint32_t kHistoryDepth = 60;  // ~6 seconds at 10 Hz

struct alignas(64) PerCoreHistory {
    float utilization_pct[kMaxCores][kHistoryDepth];
    uint8_t write_idx[kMaxCores];  // ring write position per core
};
```

Total memory: 64 × 60 × 4 + 64 = 15424 bytes = 15 KB. Cabe holgado en cache L2.

Update:
```cpp
void push_sample(PerCoreHistory& h, uint32_t core, float pct) noexcept {
    const auto idx = h.write_idx[core];
    h.utilization_pct[core][idx] = pct;
    h.write_idx[core] = (idx + 1) % kHistoryDepth;
}
```

### §5.3 — Topology layout en UI

```cpp
void draw_topology_map(const gma::HardwareTopology& topo,
                       const PerCoreHistory& hist) noexcept
{
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // Group cores by CCD
    constexpr float kCoreW = 64.0f;
    constexpr float kCoreH = 56.0f;
    constexpr float kGroupSpace = 30.0f;

    float y = origin.y;
    for (uint32_t ccd = 0; ccd < topo.ccd_count(); ++ccd) {
        // CCD header (V-Cache indicator)
        const bool has_vcache = ccd_has_vcache(topo, ccd);
        const ImU32 header_color = has_vcache ? IM_COL32(80, 160, 255, 80)
                                              : IM_COL32(120, 120, 120, 60);
        // ... draw CCD label

        float x = origin.x;
        for (uint32_t logical = 0; logical < topo.logical_core_count(); ++logical) {
            const auto& core = topo.cores[logical];
            if (core.ccd_id != ccd) continue;

            // Latest utilization
            const auto& core_hist = hist.utilization_pct[logical];
            const uint8_t w = hist.write_idx[logical];
            const uint8_t latest = (w == 0) ? kHistoryDepth - 1 : w - 1;
            const float pct = core_hist[latest];

            // Heatmap color
            const ImU32 fill = util_to_color(pct);
            dl->AddRectFilled({x, y}, {x + kCoreW, y + kCoreH}, fill, 4.0f);

            // Sparkline (last 60 samples)
            draw_sparkline(dl, x, y + kCoreH - 16, kCoreW, 16, core_hist, w);

            // Core label
            char buf[16];
            std::snprintf(buf, sizeof(buf), "C%u\n%.0f%%", logical, pct);
            dl->AddText({x + 4, y + 4}, IM_COL32_WHITE, buf);

            x += kCoreW + 4;
        }
        y += kCoreH + kGroupSpace;
    }
}
```

Patterns:
- **Direct draw list**: zero widget overhead.
- **Color from float**: tabla precomputada de gradiente o lerp directo.
- **Sparkline inline**: una pasada por sample, `AddLine` o `AddPolyline`.

Target: `< 0.5 ms / frame` para topology map completo.

### §5.4 — util_to_color branchless

```cpp
constexpr ImU32 util_to_color(float pct) noexcept {
    // Clamp 0..100
    const float p = (pct < 0.0f) ? 0.0f : (pct > 100.0f) ? 100.0f : pct;
    const float t = p * 0.01f;

    // Linear interpolation from green (0,180,80) to red (220,60,40)
    const uint8_t r = static_cast<uint8_t>(t * 220 + (1 - t) * 0);
    const uint8_t g = static_cast<uint8_t>(t * 60  + (1 - t) * 180);
    const uint8_t b = static_cast<uint8_t>(t * 40  + (1 - t) * 80);
    return IM_COL32(r, g, b, 255);
}
```

Pure FP math, ~10 cycles. Llamado 16 veces/frame = 160 cycles total. Negligible.

---

## §6 — Cross-cutting: logging, errors, threading invariants

### §6.1 — Logging post-public

Pre-public era OK con `std::fprintf(stdout, "[Ayama][Foo] ...")`. Post-public:

- **Verbose default OFF**. Solo log con `--verbose` flag o env var `AYAMA_LOG_LEVEL=debug`.
- **Structured event for parsing**: `[Ayama][2026-05-17T03:14:15Z][INFO][Classify] pid=12345 name=Foo.exe kind=Game`.
- **Rate-limit per category**: e.g. Classify log al máximo 10/s para evitar flood en sistemas con muchos procesos.
- **No PII in logs**: nombres de procesos OK, paths completos quizás. User home directory expanded → ofuscar.

Patrón:
```cpp
namespace ayama::log {

enum class Level : uint8_t { Off, Error, Warn, Info, Debug, Trace };

extern std::atomic<Level> g_threshold;

template <typename... Args>
void log(Level lvl, const char* cat, const char* fmt, Args&&... args) noexcept {
    if (lvl > g_threshold.load(std::memory_order_relaxed)) return;
    char buf[512];
    std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
    // ... write with timestamp + cat to stdout (or file if configured)
}

#define AYAMA_LOG_INFO(cat, fmt, ...) \
    ::ayama::log::log(::ayama::log::Level::Info, cat, fmt, ##__VA_ARGS__)

}
```

Threshold check con `load(relaxed)` = ~1 cycle. Si threshold filter, function return inmediato.

### §6.2 — Error handling post-public

Hoy: `std::expected<T, gma::Error>`. Continúa siendo standard, pero post-public agregar:

- **User-friendly error messages**: cada `gma::ErrorCode` con string fallback para UI.
- **Error context propagation**: añadir `gma::Error::with_context(const char*)` para chain breadcrumbs.
- **Telemetry de errores**: counter por ErrorCode publicado a SHM para "agent saw X errors in last hour".

### §6.3 — Threading invariants

Documentar **explícitamente** qué thread puede llamar qué:

| API | Thread permitido |
|---|---|
| `AgentRuntime::start()` / `stop()` | Cualquiera (signal-safe) |
| `AgentRuntime::run()` | Solo thread principal (blocking) |
| `PolicyEngine::evaluate()` | Solo thread principal (no re-entrant) |
| `ActionExecutor::apply()` / `revert()` | Solo thread principal |
| `AyamaClient::targets()` / etc. | Cualquier reader thread (seqlock) |
| `AyamaCommandSlot::seq.store(...)` | UI thread único |

Verificación en debug: `thread_local` con thread ID check al entry de cada función. En release: zero overhead (todo eliminado por compiler).

---

## §7 — Validación y benchmarks de regresión

### §7.1 — Benchmarks automatizados pre-merge

`scripts/perf_smoke_test.ps1`:

```powershell
# Run a 30s baseline + 30s with each feature enabled, fail if any
# exceeds budgets from §0.2.

$baseline = Measure-Command { & .\ayama-agent.exe --no-actions --duration 30 }
$with_graph = Measure-Command { & .\ayama-agent.exe --no-actions --duration 30 --enable-graph }

if ($with_graph.TotalSeconds -gt $baseline.TotalSeconds * 1.05) {
    Write-Error "Graph feature added > 5% wall-clock overhead. Regression."
    exit 1
}
```

Run en cada CI build de master. Si regresses, no mergear.

### §7.2 — Per-feature micro-benchmarks

Para Feature A:
```cpp
// tests/graph_microbench.cpp
TEST_CASE("graph_run_overhead") {
    Graph g;
    // Add 8 trivial nodes
    NopNode nodes[8];
    for (int i = 0; i < 8; ++i) g.add_node(nodes[i], "nop");
    g.compile();

    constexpr int kIter = 100000;
    const uint64_t t0 = gma::hal::rdtsc();
    for (int i = 0; i < kIter; ++i) g.run();
    const uint64_t t1 = gma::hal::rdtsc();

    const double avg_cycles = (t1 - t0) / static_cast<double>(kIter);
    const double avg_ns = avg_cycles / 4.2;  // approx 4.2 GHz
    REQUIRE(avg_ns < 5000.0);  // < 5 µs per graph.run() call
}
```

Threshold conservador. Si feature regresa, este test falla.

### §7.3 — Memory leak detection

Por each milestone:
- Run agent + UI por 1 hora
- Comparar RSS antes/después
- Si delta > 5 MB → leak hunt

Tooling: `Get-Process ayama-agent | Select-Object WorkingSet64` cada 10 min, log diferencias.

### §7.4 — Empirical regression test

El catálogo de 14 reports actúa como **regression test no-automatizado**. Después de cualquier cambio significativo:
- Re-run Halo CE benchmark
- Si Δ Avg FPS no es ≥ +90% (era +98%), investigar regresión
- Re-run Halo 4 benchmark
- Si Δ Avg FPS no es ≈ 0% (era 0%), investigar (algo cambió la baseline?)

Trade-off: este test requiere user con hardware idéntico, no automatizable en CI. Ejecutar manualmente antes de release major.

---

## §8 — Anti-patterns a evitar

Lista negra de errores comunes que invalidan los principios de §0:

### ❌ STL allocaciones en runtime
```cpp
// BAD
void evaluate(/*...*/) {
    std::vector<Candidate> candidates;  // ¡ALLOCATES!
    candidates.push_back(...);
}

// GOOD
void evaluate(/*...*/) {
    Candidate candidates[kMaxTargets];  // stack-allocated
    uint32_t n = 0;
    candidates[n++] = ...;
}
```

### ❌ Virtual dispatch en hot path
```cpp
// BAD
class Node {
    virtual void execute() = 0;  // vtable lookup per call
};

// GOOD
template <typename T> requires ExecutableNode<T>
void execute_thunk(void* p) noexcept {
    static_cast<T*>(p)->execute();  // inlined, monomorphic
}
```

### ❌ Mutex en producer-consumer
```cpp
// BAD
std::mutex mtx;
std::vector<Event> queue;
void push(Event e) {
    std::lock_guard g(mtx);  // contention
    queue.push_back(e);
}

// GOOD
gma::ipc::Ring<Event, 256> ring;
void push(Event e) {
    (void)ring.try_push(e);  // lock-free atomic
}
```

### ❌ String concat en log paths
```cpp
// BAD
std::string msg = "[Ayama] pid=" + std::to_string(pid);  // allocates
fprintf(stdout, "%s\n", msg.c_str());

// GOOD
fprintf(stdout, "[Ayama] pid=%u\n", pid);  // direct, no alloc
```

### ❌ Dynamic_cast / RTTI
```cpp
// BAD
if (auto* n = dynamic_cast<MetricsNode*>(node_ptr)) { /* ... */ }

// GOOD
if (slot.execute_fn == &execute_thunk<MetricsNode>) { /* ... */ }
// O usar discriminator union explícito (`type` field).
```

### ❌ Sleeping in tick loop
```cpp
// BAD
while (running) {
    do_tick();
    std::this_thread::sleep_for(100ms);  // missed wake events
}

// GOOD
while (running) {
    do_tick();
    (void)wake_event.wait(100);  // wakes early on signal
}
```

---

*Implementation strategies aprobado como base técnica para fase de showcase.
Re-evaluar §7 (benchmarks) después de Feature A implementación — los thresholds
pueden necesitar ajuste con datos reales.*
