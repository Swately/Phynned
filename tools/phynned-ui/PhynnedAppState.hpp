// tools/phynned-ui/PhynnedAppState.hpp
// POD state for the phynned-ui window.
//
// PhynnedSnapshotMini (256B) — compact agent status, published by
//   PhynnedLogicNode each tick. The large arrays (targets[], metrics[],
//   decisions[]) are read directly from PhynnedClient in the panels.
//
// PhynnedAppState (448B = 7 × 64B cache lines) — flows via Ring<PhynnedAppState>
//   from PhynnedLogicNode → RenderNode<PhynnedAppState>.
//
// Invariants:
//   sizeof(PhynnedAppState) % 64 == 0
//   sizeof(PhynnedAppState) <= 4096
//   is_trivially_copyable + is_standard_layout
//
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>

// ─────────────────────────────────────────────────────────────────────────────
// PhynnedSnapshotMini — 256B, 4 cache lines
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(8) PhynnedSnapshotMini {
    // ── Status byte flags ───────────────────────────────────────────────────
    uint8_t  agent_connected;        // 0   1 = PhynnedClient::is_connected()
    uint8_t  privilege_level;        // 1   0=None 1=Partial 2=Elevated 3=Admin
    uint8_t  baseline_active;        // 2   1 = Baseline recording in progress
    uint8_t  bench_phase;            // 3   0=idle 1=phase_a 2=phase_b 3=done

    // ── Counters ─────────────────────────────────────────────────────────────
    uint32_t target_count;           // 4
    uint32_t decision_count;         // 8
    uint32_t action_count;           // 12
    uint32_t total_migrations_obs;   // 16  sum of migrations/s across targets

    // ── Aggregates ───────────────────────────────────────────────────────────
    float    aggregate_pressure;     // 20  weighted avg pressure 0.0-2.0
    uint32_t _pad_a;                 // 24
    uint32_t _pad_b;                 // 28
    uint64_t last_sync_tsc;          // 32  TSC of last successful SHM read

    // ── Top-5 targets (most active) ───────────────────────────────────────
    uint32_t top_target_pids[5];     // 40
    uint32_t _pad_c;                 // 60  → aligns names to offset 64
    char     top_target_names[5][32];// 64  null-terminated, ASCII safe

    // ── Extended runtime status (previously padding) ──────────────────────
    uint32_t bad_count;              // 224  per-game bad-list entry count
    uint8_t  deep_idle;              // 228  1 if agent is in DeepIdle state
    uint8_t  watchdog_ok;            // 229  1 = watchdog healthy; 0 = stall detected
    uint8_t  _pad_d1[2];             // 230  align next uint32_t to offset 232
    // ── CCD Load Defense telemetry ────────────────────────────
    uint32_t ccd_defense_count;      // 232  # processes evicted from V-Cache CCD
    float    ccd_defense_cpu_pct;    // 236  summed CPU% of those processes
    // ── Hardware classification (v1.0) ─────────────────────────────────────
    // Mirrors PhynnedStateHeader::cpu_class etc. — set once at agent startup
    // and read by every UI tick. UI renders arch-aware text from these bytes.
    uint8_t  cpu_class;              // 240  policy::CpuClass enum:
                                     //        0=Unknown 1=X3DSingle 2=X3DDual
                                     //        3=HybridIntel 4=MultiCCXNoX3D 5=SingleCCD
    uint8_t  ccd_count;              // 241  # CCDs detected (Intel = 1)
    uint8_t  p_core_count;           // 242  # P-cores (Intel hybrid only; else 0)
    uint8_t  e_core_count;           // 243  # E-cores (Intel hybrid only; else 0)
    // ── Runtime control state ─────────────────────────────────
    uint8_t  policies_paused;        // 244  1 if agent.executor.apply() gated off
    uint8_t  _pad_d2a[3];            // 245  align total_tracked to offset 248
    // ── MASS-router detection scale ───────────────────────────
    uint32_t total_tracked;          // 248  full internal touchable count observed
                                     //        by the agent (≥ target_count). The
                                     //        table shows the top target_count of
                                     //        these; the rest are observed only.
    // ── MR-2 background corral mode ───────────────────────────
    uint8_t  corral_live;            // 252  1 = LIVE switch ON (0 = DRY-RUN default)
    uint8_t  corral_coexist_block;   // 253  1 = coexistence optimizer forces DRY-RUN
    // ── W4 global profile (use-modes) ─────────────────────────
    uint8_t  profile;                // 254  config::Profile: 0=Monitor 1=Games
                                     //        2=GamesCorral 3=Full
    uint8_t  _pad_d2b;               // 255  fill to 256B
};
static_assert(sizeof(PhynnedSnapshotMini) == 256u,
    "PhynnedSnapshotMini must be exactly 256B");
static_assert(offsetof(PhynnedSnapshotMini, total_tracked) == 248u,
    "total_tracked must be 4-byte aligned at offset 248");
static_assert(offsetof(PhynnedSnapshotMini, profile) == 254u,
    "profile byte carved from _pad_d2b @254");
static_assert(std::is_trivially_copyable_v<PhynnedSnapshotMini>);
static_assert(std::is_standard_layout_v<PhynnedSnapshotMini>);

// ─────────────────────────────────────────────────────────────────────────────
// PhynnedAppState — 448B = 7 × 64B cache lines
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(64) PhynnedAppState {
    // ── Snapshot (256B) ──────────────────────────────────────────────────────
    PhynnedSnapshotMini snap;             // 0..255

    // ── UI control state (4B) ────────────────────────────────────────────────
    uint8_t  op_mode;                   // 256  0=Auto 1=Assist 2=Manual
    uint8_t  etw_active;                // 257  1 = ETW session running
    uint8_t  frame_observer_active;     // 258  1 = PresentMon-style frame data
    uint8_t  probe_complete;            // 259  1 = HW probe done at startup

    // ── Self-metrics from phynned-agent (24B) ─────────────────────────────────
    float    self_cpu_pct;              // 260  agent own CPU %
    float    self_rss_mb;               // 264  agent RSS in MB
    uint32_t tick_interval_ms;          // 268  current adaptive tick interval
    uint32_t self_pin_core;             // 272  core index where agent is pinned
    uint32_t win_width;                 // 276  last known window width
    uint32_t win_height;                // 280  last known window height
    uint32_t agent_pid;                 // 284  PID of the agent process
    uint32_t _pad_e;                    // 288

    // ── Version string (32B) ─────────────────────────────────────────────────
    char     agent_version[32];         // 292  e.g. "Phynned 0.1.0-experimental"

    // ── Last error message (64B) ─────────────────────────────────────────────
    char     last_error[64];            // 324  "" = no error

    // ── Padding to 448B ──────────────────────────────────────────────────────
    uint8_t  _pad_f[60];               // 388..447
};
static_assert(sizeof(PhynnedAppState) == 448u,
    "PhynnedAppState must be exactly 448B");
static_assert(sizeof(PhynnedAppState) % 64u == 0u,
    "PhynnedAppState must be a multiple of 64B");
static_assert(sizeof(PhynnedAppState) <= 4096u,
    "PhynnedAppState must fit in a Ring slot");
static_assert(std::is_trivially_copyable_v<PhynnedAppState>);
static_assert(std::is_standard_layout_v<PhynnedAppState>);
// Made with my soul - Swately <3
