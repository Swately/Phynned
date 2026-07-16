// framework/stigmergy/include/phyriad/stigmergy/Classifier.hpp
// phyriad::stigmergy::Classifier<Signal, Action> — emergent decision policy.
//
// A Classifier is the "brain" of a stigmergic system: given an incoming
// Signal (a task, a sensor reading, an event) and the current pheromone
// field state (observed from Field<T> or Pheromone<T,N>), it produces
// an Action (route this task to worker K, classify this event as tier X).
//
// The Classifier itself OWNS no state of the field — it only reads it.
// Concrete subclasses hold references/pointers to whatever Field<T>s,
// Pheromone<T,N>s they consult. This avoids forcing a fixed shape: some
// classifiers need 1 field + 0 pheromones, others need 0 fields + 3
// pheromones, etc. The pattern is composition over template-parameters.
//
// Performance contract:
//   decide() p99 < 200 ns end-to-end (including any field/pheromone reads)
//   The 200 ns budget is for a "typical" decision: read 1 field (50 ns) +
//   read N pheromone slots (5 ns × N) + decision logic (50 ns).
//
// Why a base class and not just a concept:
//   * Allows runtime polymorphism if a system needs to swap classifiers
//     (e.g. A/B testing two routing policies under the same dispatcher).
//   * Provides a stable extension point for future hooks (telemetry,
//     replay capture) without breaking subclasses.
//   * Even with virtual call overhead (~3 ns on modern x86), the cost is
//     negligible vs the cache misses of the field/pheromone reads.
//
// If you need a static (compile-time) classifier, just write a regular
// function/functor and pass it where the Classifier-using code expects
// one. The base class is for cases where you want the type-erasure.
//
// Stigmergy as first-class pillar.
// §Strategies:  see PHYRIAD_PUBLICATION_IMPLEMENTATION_STRATEGIES.md §1.1
#pragma once

namespace phyriad::stigmergy {

/// Abstract base class for an emergent decision policy.
///
/// Concrete subclasses override `decide()` to produce an Action from an
/// input Signal, consulting whatever Field/Pheromone state they hold
/// internally (typically passed in via the subclass constructor).
///
/// Usage pattern:
///
///   struct RouteDecision { uint32_t worker_id; bool overflow; };
///
///   class FillBasedRouter
///       : public phyriad::stigmergy::Classifier<Task, RouteDecision> {
///       phyriad::stigmergy::Pheromone<uint8_t, 32>& fill_;
///   public:
///       explicit FillBasedRouter(decltype(fill_) p) noexcept : fill_{p} {}
///
///       [[nodiscard]] RouteDecision
///       decide(Task const& t) noexcept override {
///           auto snapshot = fill_.read_all();
///           uint8_t min_fill = 100;
///           uint32_t best = 0;
///           for (uint32_t i = 0; i < snapshot.size(); ++i) {
///               if (snapshot[i] < min_fill) {
///                   min_fill = snapshot[i];
///                   best = i;
///               }
///           }
///           return RouteDecision{best, min_fill >= 95u};
///       }
///   };
template <typename Signal, typename Action>
class Classifier {
public:
    using signal_type = Signal;
    using action_type = Action;

    Classifier()                             noexcept = default;
    virtual ~Classifier()                    noexcept = default;
    Classifier(Classifier const&)            = delete;
    Classifier& operator=(Classifier const&) = delete;
    Classifier(Classifier&&)                 = delete;
    Classifier& operator=(Classifier&&)      = delete;

    /// Produce an Action from a Signal, consulting the held field state.
    ///
    /// MUST be noexcept — a throw from a hot dispatch path is unacceptable.
    /// Should complete in < 200 ns p99 for typical workloads (this is
    /// part of the performance contract documented in the master plan).
    ///
    /// Implementations that need to mutate internal counters (e.g. for
    /// learning policies) may do so via `mutable` members or atomic
    /// fetch_add — but should NOT regress the < 200 ns SLO.
    [[nodiscard]] virtual Action
    decide(Signal const& signal) noexcept = 0;
};

} // namespace phyriad::stigmergy
// Made with my soul - Swately <3
