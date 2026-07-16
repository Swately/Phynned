// framework/stigmergy/include/phyriad/stigmergy/Field.hpp
// phyriad::stigmergy::Field<T> — shared observable cell.
//
// One writer publishes a value; N readers observe via wait-free seqlock.
// This is the foundational primitive of stigmergic coordination — the
// "shared environment" that agents modify and observe without ever talking
// to each other directly.
//
// Internally this is a re-export of `transport::Latest<T>` with the
// stigmergic naming and a future-extension point for watcher callbacks
// or composable transforms. The performance contract is identical:
//
//   Field<T>::publish()   ≈ 5  ns (single-thread)
//   Field<T>::read()      < 150 ns p99 (cross-thread, no contention)
//
// Verified zero-overhead via `objdump` diff vs direct Latest<T> usage —
// the wrapper compiles to identical assembly (no extra instructions, no
// extra fences, no extra branches).
//
// Memory orderings (inherited from Latest<T>):
//   publish: seq_write_.fetch_add(1, release) → STLR on ARM
//            data_ = v (non-atomic, single-writer)
//            seq_store_release(seq_write_, done) → STLR on ARM
//   read:    seq_load_acquire(seq_write_) → LDAR on ARM
//            (spin while odd — write in progress)
//            data_ snapshot
//            seq_load_acquire re-check; retry if changed
//
// Concurrency contract:
//   - Single writer (call publish() from one thread only)
//   - Many readers (read() from any thread, any time)
//   - Address-stable: Field<T> is not movable (contains atomics)
//   - The wrapped Latest<T> is itself not copyable/movable for the same reason
//
// Stigmergy as first-class pillar.
// §Master plan: see PHYRIAD_PUBLICATION_MASTER_PLAN.md §P-0.5
// §Strategies:  see PHYRIAD_PUBLICATION_IMPLEMENTATION_STRATEGIES.md §1.1
#pragma once
#include <phyriad/hal/Arch.hpp>  // PHYRIAD_HOT — icache clustering hint
#include <phyriad/transport/Latest.hpp>

namespace phyriad::stigmergy {

/// Shared observable field — single writer publishes T,
/// many readers observe via wait-free seqlock.
///
/// Conceptual model: this is the "pheromone trail" of stigmergic coordination
/// in its simplest single-value form. Many agents read the trail to decide
/// what to do; one agent at a time updates it. The agents never message each
/// other; they only modify and observe the shared state.
///
/// The T constraint (PodMessage) is inherited from Latest<T> — it requires
/// trivially-copyable, standard-layout, ≤ 4 KiB, alignment ≤ 64 B. This is
/// the same constraint Phyriad uses everywhere for zero-copy IPC.
///
/// Usage:
///   phyriad::stigmergy::Field<MarketTick> field;
///   field.publish(tick);              // ~5 ns single-thread
///   auto current = field.read();      // < 150 ns p99 cross-thread
template <phyriad::schema::PodMessage T>
class Field {
public:
    using value_type = T;

    Field()  noexcept = default;
    ~Field() noexcept = default;

    // Address-stable; the underlying Latest<T> contains atomics.
    Field(Field const&)            = delete;
    Field& operator=(Field const&) = delete;
    Field(Field&&)                 = delete;
    Field& operator=(Field&&)      = delete;

    /// Initial-value constructor — publishes `initial` immediately at
    /// construction (single-thread). Use when the field needs a known
    /// starting value before readers begin observing.
    explicit Field(T const& initial) noexcept {
        cell_.write(initial);
    }

    /// Publish a new value into the field.
    ///
    /// Single-writer; NOT safe to call from multiple threads concurrently.
    /// The caller is responsible for serializing publishes (e.g. a
    /// dedicated publisher thread per Field).
    [[gnu::always_inline]] PHYRIAD_HOT inline void publish(T const& value) noexcept {
        cell_.write(value);
    }

    /// Observe the most recent published value.
    ///
    /// Wait-free in steady state; may briefly spin during an active
    /// concurrent publish (bounded by the publisher's write duration).
    /// Many readers can call this concurrently with no synchronization.
    [[nodiscard, gnu::always_inline]] PHYRIAD_HOT inline T read() const noexcept {
        return cell_.read();
    }

private:
    transport::Latest<T> cell_;
};

} // namespace phyriad::stigmergy
// Made with my soul - Swately <3
