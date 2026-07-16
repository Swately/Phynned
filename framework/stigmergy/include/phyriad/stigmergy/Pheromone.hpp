// framework/stigmergy/include/phyriad/stigmergy/Pheromone.hpp
// phyriad::stigmergy::Pheromone<T, N> — fixed-size pheromone array.
//
// N agents each own slots_[i] (write-only by agent i); any reader can
// observe any/all slots. This is the canonical "pheromone deposit"
// pattern: each agent leaves a trace at their own index, and the
// emergent decision logic reads the whole field to choose its action.
//
// Internally a `std::array<alignas-padded atomic<T>, N>`. Each slot is
// cache-line padded so adjacent agents on different CCDs don't ping-pong
// the same cache line. This makes the pattern scale beyond a single CCD
// (validated by `bench_stigmergy_workflow` scaling sweep).
//
// Performance contract:
//   deposit(i, v)         < 10 ns (relaxed atomic store, single-writer per slot)
//   read(i)               <  5 ns (relaxed atomic load)
//   read_all() per slot   <  5 ns (linear scan, cache-line-friendly)
//
// Why relaxed memory order:
//   Stigmergic coordination is by design eventually-consistent. A reader
//   that sees stale data still makes a valid decision — it just might be
//   based on a slightly older snapshot. The reader will re-read on its
//   next iteration and converge.
//
//   If you need strict ordering (e.g. "all deposits before timestamp T
//   are observed"), use Field<T> instead — that gives you release/acquire
//   semantics via Latest<T>'s seqlock.
//
// Stigmergy as first-class pillar.
// §Strategies:  see PHYRIAD_PUBLICATION_IMPLEMENTATION_STRATEGIES.md §1.1
#pragma once
#include <phyriad/hal/Arch.hpp>       // PHYRIAD_HOT — icache clustering hint
#include <phyriad/hal/Cacheline.hpp>
#include <phyriad/hal/MemoryOrder.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace phyriad::stigmergy {

namespace detail {

// Constraint: Pheromone slots must hold a type that's storable in
// std::atomic<T>. In practice this means trivially-copyable + small
// (≤ 8 B is lock-free on x86-64; larger may use a kernel-spin).
template <typename T>
concept AtomicStorable =
    std::is_trivially_copyable_v<T> &&
    std::is_copy_constructible_v<T> &&
    sizeof(T) <= 16;   // 16 B = max DWCAS on x86-64

} // namespace detail

/// Pheromone field — N atomic slots, one per agent.
///
/// Agent `i` deposits into slot `i` (single writer per slot). Any reader
/// can read any slot at any time. The pheromone metaphor: each agent
/// leaves a "trail" at its position, and the classifier observes the
/// pattern of all trails to decide globally.
///
/// Memory layout: each slot is cache-line-padded via `alignas(kDestructivePad)`
/// to prevent false-sharing across agents. The padding cost is amortized
/// across N agents — for N ≥ 8 the wasted space is negligible relative
/// to the contention saved.
///
/// Concurrency contract:
///   - Single writer per slot (agent i owns slot i exclusively).
///   - Many concurrent readers OK (any thread can read any slot).
///   - read_all() takes a snapshot; the values may not all reflect the
///     same point in time (no atomic group-snapshot guarantee).
///
/// Usage:
///   phyriad::stigmergy::Pheromone<uint8_t, 32> fill_pct;
///   // Worker thread i:
///   fill_pct.deposit(i, static_cast<uint8_t>(my_fill * 100));
///   // Classifier thread:
///   auto snapshot = fill_pct.read_all();
///   for (uint8_t v : snapshot) { /* decide */ }
template <detail::AtomicStorable T, std::size_t N>
class Pheromone {
    static_assert(N > 0, "Pheromone requires N > 0 slots");
    static_assert(N <= 65536u, "Pheromone N > 65536 is impractical; reconsider design");

public:
    using value_type = T;
    static constexpr std::size_t size_v = N;

    Pheromone()  noexcept = default;
    ~Pheromone() noexcept = default;

    // Address-stable; contains atomics.
    Pheromone(Pheromone const&)            = delete;
    Pheromone& operator=(Pheromone const&) = delete;
    Pheromone(Pheromone&&)                 = delete;
    Pheromone& operator=(Pheromone&&)      = delete;

    /// Deposit (write) a new pheromone value at slot `agent_id`.
    ///
    /// Single-writer per slot. NOT safe to call from multiple threads
    /// for the same agent_id concurrently. Different agent_ids may
    /// deposit concurrently with no synchronization.
    ///
    /// `agent_id ≥ N` is a no-op (defensive — caller bug, not a crash).
    [[gnu::always_inline]] PHYRIAD_HOT inline void
    deposit(std::size_t agent_id, T value) noexcept {
        if (agent_id >= N) [[unlikely]] return;
        hal::stat_store_relaxed(slots_[agent_id].value, value);
    }

    /// Read the most recently deposited value at slot `agent_id`.
    ///
    /// Wait-free. Many concurrent readers OK. Returns default-constructed
    /// T (typically zero) for out-of-range agent_id.
    [[nodiscard, gnu::always_inline]] PHYRIAD_HOT inline T
    read(std::size_t agent_id) const noexcept {
        if (agent_id >= N) [[unlikely]] return T{};
        return hal::stat_load_relaxed(slots_[agent_id].value);
    }

    /// Read all N slots into a std::array snapshot.
    ///
    /// The slots are read sequentially (linear scan). Values may not all
    /// reflect the same instant — concurrent deposits during the scan
    /// land partially. For pheromone-style decisions, this is fine:
    /// readers tolerate eventual consistency by design.
    ///
    /// Cache behavior: the loop accesses N cache-line-padded slots, so
    /// N reads touch N cache lines. For N=32 this is 4 KiB of cache —
    /// fits comfortably in L1 (32 KiB typical). For N=256 it spills to
    /// L2, doubling latency but still ≪ 1 µs.
    [[nodiscard]] PHYRIAD_HOT std::array<T, N> read_all() const noexcept {
        std::array<T, N> out;
        for (std::size_t i = 0; i < N; ++i) {
            out[i] = hal::stat_load_relaxed(slots_[i].value);
        }
        return out;
    }

    /// Reset a single slot to the default-constructed T value.
    ///
    /// Useful when an agent goes offline and its pheromone should
    /// be "forgotten" by the classifier. Single-writer (only agent_id
    /// or the orchestrator should reset).
    [[gnu::always_inline]] inline void
    clear(std::size_t agent_id) noexcept {
        if (agent_id >= N) [[unlikely]] return;
        hal::stat_store_relaxed(slots_[agent_id].value, T{});
    }

    /// Reset all slots to T{}. NOT safe to call concurrent with deposits.
    /// Typically called from a setup or teardown phase.
    void clear_all() noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            hal::stat_store_relaxed(slots_[i].value, T{});
        }
    }

    /// Compile-time access to the slot count.
    [[nodiscard]] static constexpr std::size_t size() noexcept { return N; }

private:
    // Each slot lives on its own cache line to prevent false sharing
    // when agents on different cores deposit concurrently.
    struct alignas(hal::kDestructivePad) Slot {
        std::atomic<T> value{T{}};
    };
    std::array<Slot, N> slots_{};
};

} // namespace phyriad::stigmergy
// Made with my soul - Swately <3
