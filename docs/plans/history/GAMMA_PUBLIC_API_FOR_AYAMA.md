> **📦 Archived planning document — historical.**
> From Phyriad's pre-rebrand era (the project was codenamed *gamma* / *gma*).
> Published for transparency about how the project was actually built — **not**
> a current specification. Version numbers and performance claims here
> (including any "vs Folly" / "production-ready" framing) reflect the project's
> state at the time of writing and may since have been revised or **retracted**;
> the project has reset to `v0.1.0-experimental`. For current status see the
> repo `README.md` and `docs/planning/`. For benchmark-claim validation status
> see `docs/planning/TEST_INVENTORY.md`.

# Gamma Public API for Ayama Consumers

*Ayama v0.1 · Gamma Framework 1.0.0*

This document describes the Gamma pillars consumed by Ayama. Ayama is a **non-invasive consumer** — it only uses public headers and never modifies Gamma source files.

---

## 1. `gamma/topology` — Hardware detection

**Target**: `gamma_topology`

### Core types

```cpp
#include <gamma/topology/HardwareTopology.hpp>

// Probe the full CPU/GPU/NUMA/cache topology.
// Singleton: OS calls execute once per process.
auto result = gma::HardwareTopology::probe();  // → std::expected<HardwareTopology, std::string>

// Per-core descriptor
gma::CoreInfo {
    uint32_t logical_id;       // OS index (SetThreadAffinityMask)
    uint32_t ccd_id;           // AMD chiplet die
    bool     has_v_cache;      // AMD 3D V-Cache
    bool     is_efficiency_core; // Intel E-core
    uint8_t  efficiency_class; // 0=E, 1+=P
    uint32_t max_freq_mhz;
};
```

### `hw::` namespace (singleton cached, noexcept)

```cpp
namespace gma::hw {
    std::vector<uint32_t> v_cache_cores() noexcept;  // AMD X3D cores (empty if N/A)
    std::vector<uint32_t> p_cores()       noexcept;  // Intel P-cores (all cores on non-hybrid)
    std::vector<uint32_t> e_cores()       noexcept;  // Intel E-cores (empty on non-hybrid)
    std::vector<uint32_t> ccd_cores(uint32_t ccd_id) noexcept;  // cores in given CCD
    bool pin_current_thread(uint32_t logical_id) noexcept;
    bool elevate_thread_rt(bool time_critical = true) noexcept;
}
```

**Used by Ayama for:** Self-pinning, AutoPolicySelector, PolicyEngine::register_default_rules.

---

## 2. `gamma/hal` — Hardware abstraction (timestamps)

**Target**: `gamma_hal`

```cpp
#include <gamma/hal/Timestamp.hpp>

uint64_t gma::hal::rdtsc() noexcept;                 // raw TSC
uint64_t gma::hal::calibrate_tsc_freq() noexcept;    // TSC ticks per second
```

**Used by Ayama for:** Adaptive tick timing, TSC-based age calculations in AutoRevertGuard and ClassificationCache.

---

## 3. `gamma/tuning` — Privilege detection

**Target**: `gamma_tuning`

```cpp
#include <gamma/tuning/PrivilegeCheck.hpp>

namespace gma::tuning {
    enum class PrivilegeLevel : uint8_t { None=0, Partial=1, Elevated=2, Admin=3 };
    PrivilegeLevel check_privilege_level() noexcept;
}
```

**Used by Ayama for:** Deciding whether to start ETW / apply affinity changes.

---

## 4. `gamma/ui` — Application framework

**Target**: `gamma_ui`

```cpp
#include <gamma/ui/Application.hpp>
#include <gamma/ui/ApplicationConfig.hpp>
#include <gamma/ui/RenderNode.hpp>

// Entry point pattern (used by ayama-ui):
gma::ui::Application::run(cfg, [](NodeRegistry&, DslGraphBuilder&, IRenderBackend&, FrameArena&) {
    // wire nodes here
});

// Typed render node:
gma::ui::RenderNode<T>  // calls draw_fn(const T& state) each frame via ImGui
```

**Used by Ayama for:** ayama-ui standalone window.

---

## 5. `gamma/node` — Port primitives

**Target**: `gamma_node`

```cpp
#include <gamma/node/Port.hpp>

gma::node::Inlet<T>   // receives values of type T (polling: .receive() → optional<T>)
gma::node::Outlet<T>  // publishes values of type T (.publish(value))
```

**Used by Ayama for:** AyamaLogicNode port wiring.

---

## 6. `gamma/schema` — Error type

**Target**: `gamma_schema`

```cpp
#include <gamma/schema/Error.hpp>

struct gma::Error {
    gma::ErrorCode code;
    const char*    message;
};
```

**Used by Ayama for:** Return types on all fallible public APIs.

---

## Consumption contract

| Rule | Rationale |
|------|-----------|
| No Gamma source files modified | Ayama is an add-on, not a fork |
| All `hw::` results cached by Gamma on first call | Ayama does not re-probe topology at runtime |
| All `gma::Error` returns checked | Ayama degrades gracefully on any Gamma failure |
| `gamma_ui` only linked in `ayama-ui` target | Core agent has no UI dependency |

---

*Last updated: May 2026 — Ayama v0.1*
