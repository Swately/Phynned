// framework/scheduler/include/phyriad/scheduler/PlacementPolicy.hpp
// Runtime thread-scheduling policies for the Phyriad placement engine.
//
// Four orthogonal policies:
//
//   Pinned          — hard affinity; thread never migrates after initial placement.
//                     Suitable for ultra-low-latency V-Cache-pinned nodes.
//
//   SoftAffine      — advisory affinity only; OS may migrate freely.
//                     Trades cache-locality determinism for flexibility.
//
//   WorkStealing    — pool of threads; any idle thread executes any ready node.
//                     Maximises throughput under uneven load at cost of locality.
//
//   StickyThenSteal — DEFAULT. Thread is affinity-bound while its node is active;
//                     after idle_threshold_us of inactivity it becomes stealable.
//                     Balances p99 latency (sticky phase) with utilisation (steal).
//
// PlacementPolicy is a flat discriminated struct (not std::variant) so it stays
// trivially copyable and fits in two cache words.
//

#pragma once
#include <cstdint>
#include <type_traits>

namespace phyriad::scheduler {

// ── PlacementPolicyKind ───────────────────────────────────────────────────────
enum class PlacementPolicyKind : uint8_t {
    Pinned          = 0,
    SoftAffine      = 1,
    WorkStealing    = 2,
    StickyThenSteal = 3,
};

// ── Individual policy tag structs ─────────────────────────────────────────────
// Used as typed constructors for PlacementPolicy (see named constructors below).

struct PinnedPolicy {
    static constexpr PlacementPolicyKind kind = PlacementPolicyKind::Pinned;
};

struct SoftAffinePolicy {
    static constexpr PlacementPolicyKind kind = PlacementPolicyKind::SoftAffine;
};

struct WorkStealingPolicy {
    static constexpr PlacementPolicyKind kind = PlacementPolicyKind::WorkStealing;
    uint32_t pool_size{4};   // number of threads in the stealing pool
};

struct StickyThenStealPolicy {
    static constexpr PlacementPolicyKind kind = PlacementPolicyKind::StickyThenSteal;
    uint32_t idle_threshold_us{100};  // steal eligibility after N µs of inactivity
};

// ── PlacementPolicy ───────────────────────────────────────────────────────────
// Flat discriminated struct — trivially copyable, 12 bytes.
// Default-constructed to StickyThenStealPolicy with idle_threshold_us = 100.
struct PlacementPolicy {
    PlacementPolicyKind kind{PlacementPolicyKind::StickyThenSteal};
    uint8_t  _pad[3]{};
    uint32_t pool_size{4};            // WorkStealing: thread pool count
    uint32_t idle_threshold_us{100};  // StickyThenSteal: µs before steal eligibility

    // ── Named constructors ────────────────────────────────────────────────────
    [[nodiscard]] static constexpr PlacementPolicy pinned() noexcept {
        PlacementPolicy p;
        p.kind = PlacementPolicyKind::Pinned;
        return p;
    }

    [[nodiscard]] static constexpr PlacementPolicy soft_affine() noexcept {
        PlacementPolicy p;
        p.kind = PlacementPolicyKind::SoftAffine;
        return p;
    }

    [[nodiscard]] static constexpr PlacementPolicy work_stealing(uint32_t pool = 4) noexcept {
        PlacementPolicy p;
        p.kind      = PlacementPolicyKind::WorkStealing;
        p.pool_size = pool;
        return p;
    }

    [[nodiscard]] static constexpr PlacementPolicy sticky_then_steal(
            uint32_t threshold_us = 100) noexcept {
        PlacementPolicy p;
        p.kind                = PlacementPolicyKind::StickyThenSteal;
        p.idle_threshold_us   = threshold_us;
        return p;
    }
};
static_assert(std::is_trivially_copyable_v<PlacementPolicy>);
static_assert(sizeof(PlacementPolicy) == 12);

} // namespace phyriad::scheduler
// Made with my soul - Swately <3
