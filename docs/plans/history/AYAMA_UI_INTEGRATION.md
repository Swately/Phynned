> **📦 Archived planning document — historical.**
> From Phyriad's pre-rebrand era (the project was codenamed *gamma* / *gma*).
> Published for transparency about how the project was actually built — **not**
> a current specification. Version numbers and performance claims here
> (including any "vs Folly" / "production-ready" framing) reflect the project's
> state at the time of writing and may since have been revised or **retracted**;
> the project has reset to `v0.1.0-experimental`. For current status see the
> repo `README.md` and `docs/planning/`. For benchmark-claim validation status
> see `docs/planning/TEST_INVENTORY.md`.

# Ayama UI Integration Guide

*ayama-ui v0.1 · Gamma Framework 1.0.0*

This document explains how `ayama-ui` integrates with `ayama-agent` and the Gamma rendering framework.

---

## Architecture overview

```
ayama-agent.exe          ayama-ui.exe (ayama.exe)
─────────────────        ─────────────────────────────────────────
AgentRuntime             AyamaLogicNode
  │                        │  in_input  (InputEvent)
  │  SHM (1 MB)            │  in_window (WindowState)
  │  "Local\AyamaAgent.v1" │  out_state (AyamaAppState)
  │◄──────────────────────►│
  │  seqlock write         │  AyamaClient.connect()
  │  magic = 0xAYAMA01     │  reads: targets[], metrics[],
                           │          decisions[], action_log[]
                           │          state (privilege, bad_count, …)
                           │
                           ▼
                         RenderNode<AyamaAppState>
                           draw_widgets(state)
                             ├── draw_dashboard_panel(s, ac)
                             ├── draw_targets_panel(s, ac)
                             ├── draw_policies_panel(s, ac)
                             ├── draw_actions_panel(s, ac)
                             ├── draw_bench_panel(s, ac)
                             └── draw_advanced_panel(s, ac)
```

---

## Wire graph

```
"ui" outlet 0 (InputEvent)   → "logic" inlet 0
"ui" outlet 1 (WindowState)  → "logic" inlet 1
"logic" outlet 0 (AyamaAppState) → "render" inlet 1
```

The render node receives `AyamaAppState` and calls `draw_widgets()` each frame.

---

## AyamaAppState

`AyamaAppState` (~4 KB max) is the state object flowing logic → render:

```
AyamaAppState {
    AyamaSnapshotMini snap;   // 256B — agent status (targets, pressure, etc.)
    agent_version[32];        // "Ayama 0.1.0"
    agent_pid;                // agent's PID
    op_mode;                  // 0=Auto, 1=Assist, 2=Manual
    etw_active;               // ETW session running?
    snap.bad_count;           // exes on bad-list
    snap.deep_idle;           // DeepIdle state active?
    snap.watchdog_ok;         // internal watchdog OK?
    self_cpu_pct;             // agent CPU% (anti-parasitic budget)
    self_rss_mb;              // agent RAM (MB)
    tick_interval_ms;         // current adaptive tick period
    self_pin_core;            // core agent is pinned to
    win_width, win_height;    // window dimensions
    last_error[128];          // last error string (empty = no error)
}
```

---

## Panel contract (§Block 4.6)

Every panel follows this signature:

```cpp
inline void draw_<name>_panel(
    const AyamaAppState&       s,   // snapshot from logic node
    const ayama::ipc::AyamaClient* ac // may be nullptr if agent not running
) noexcept;
```

Rules:
- If `ac == nullptr` or `!s.snap.agent_connected`: show a graceful fallback.
- Read arrays directly from `ac->targets()`, `ac->metrics()`, `ac->decisions()`, `ac->action_log()`.
- **Never** write to SHM via the client (read-only view).
- Zero heap allocation in the draw loop.
- ImGui ID namespacing: use `##panel_name_widget_id` to avoid conflicts.

---

## System tray (§9.5)

`AyamaTrayIcon` is integrated into `AyamaLogicNode`. It creates a message-only
HWND internally for receiving Shell callbacks. No main HWND is needed.

State → icon color mapping:

| TrayState | Color  | Meaning |
|-----------|--------|---------|
| Green     | 🟢     | Agent active, optimisations applied |
| Yellow    | 🟡     | Assist mode — awaiting user confirmation |
| Orange    | 🟠     | Running without admin (degraded) |
| Red       | 🔴     | Regression detected, policy reverted |
| Gray      | ⚫     | Agent not running |

Toasts are fired on:
- First connection (Gray → Green/Orange)
- Regression detection (any → Red)
- New process detected while connected

---

## Adding a new panel

1. Create `ayama/tools/ayama-ui/widgets/my_panel.hpp`.
2. Implement `draw_my_panel(const AyamaAppState& s, const ayama::ipc::AyamaClient* ac)`.
3. `#include "widgets/my_panel.hpp"` in `main.cpp`.
4. Add `if (ImGui::BeginTabItem("My Panel")) { draw_my_panel(s, ac); ImGui::EndTabItem(); }` in `draw_widgets()`.

---

## IPC command flow (planned for v0.2)

Currently, all UI panels are **read-only**. In v0.2, buttons that send commands
to the agent (Revert All, Start Bench, Force Pin) will use a write-side SHM ring:

```
ayama-ui  →  CommandRing (SHM)  →  ayama-agent  →  ActionExecutor
```

The agent polls the CommandRing each tick and dispatches commands.

---

*Last updated: May 2026 — Ayama v0.1*
