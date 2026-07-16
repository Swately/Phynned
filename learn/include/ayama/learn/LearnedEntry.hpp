// apps/ayama/learn/include/ayama/learn/LearnedEntry.hpp
// LearnedEntry — one row in %LOCALAPPDATA%\Ayama\memory.toml.
//
// Represents a validated per-game + per-hardware policy result.
// The agent persists this after a successful A/B test and loads it on
// next run to skip the discovery phase ("first-time → measure, next-time → apply").
//
// IPC-safe: trivially_copyable, standard_layout, fixed-size.
//
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace ayama::learn {

/// One validated per-game policy record.
struct alignas(8) LearnedEntry {
    // ── Key: (exe, hardware_id) uniquely identifies a learned rule ────────
    char     exe[64];              ///< Game executable (e.g. "Cyberpunk2077.exe").
    char     hardware_id[32];      ///< CPU class ID (e.g. "amd-x3d-2ccd").

    // ── Validated result ──────────────────────────────────────────────────
    char     best_action[32];      ///< Action tag (e.g. "pin_v_cache_ccd").
    uint64_t best_core_mask;       ///< Exact core mask used. 0 = rule default.

    // ── Measurement ────────────────────────────────────────────────────────
    float    improvement_pct;      ///< P99 frametime improvement (positive = better).
    uint32_t sample_count;         ///< Number of A/B runs that confirmed this.

    // ── Freshness ─────────────────────────────────────────────────────────
    char     last_validated[32];   ///< ISO-8601 timestamp of last validation.

    // ── User override ─────────────────────────────────────────────────────
    bool     user_locked;          ///< True → skip re-evaluation.
    uint8_t  _pad[15];             ///< Pad to 192B (3 cache lines).
};

static_assert(sizeof(LearnedEntry) == 192u,
    "LearnedEntry must be 192 bytes (3 cache lines)");
static_assert(std::is_trivially_copyable_v<LearnedEntry>);
static_assert(std::is_standard_layout_v<LearnedEntry>);

} // namespace ayama::learn
// Made with my soul - Swately <3
