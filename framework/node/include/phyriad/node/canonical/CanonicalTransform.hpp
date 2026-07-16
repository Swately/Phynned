// framework/node/include/phyriad/node/canonical/CanonicalTransform.hpp
// Canonical Transform node — validates Transform<N>, Stateful<N>, and mixin
// concepts are satisfiable with a concrete type (R3: zero stubs).
//
// CanonicalTransform is NOT production code. It serves as compile-time proof
// that Transform + Stateful + HasOnStart/HasOnStop work together correctly.
//
// Satisfies: Transform<CanonicalTransform>  (= Source AND Sink)
//            Stateful<CanonicalTransform>   (= Checkpointable)
//            HasOnStart, HasOnStop
//
// Behaviour: identity transform — forwards input to output unchanged.
// State: counts messages processed (uint64_t msg_count).
//

#pragma once
#include "../Port.hpp"
#include "../Lifecycle.hpp"
#include "../Categories.hpp"
#include "../Node.hpp"
#include <phyriad/schema/PodMessage.hpp>
#include <expected>

namespace phyriad::node::canonical {

struct CanonicalTransform {
    // ── Port type declarations ────────────────────────────────────────────
    using input_type  = schema::SampleTick;
    using output_type = schema::SampleTick;

    // ── Port accessors ────────────────────────────────────────────────────
    [[nodiscard]] Outlet<output_type>& outlet() noexcept { return outlet_; }
    [[nodiscard]] Inlet<input_type>&   inlet()  noexcept { return inlet_;  }

    // ── Sink: message handler ─────────────────────────────────────────────
    // Identity transform: forwards msg downstream unchanged.
    [[nodiscard]] auto on_message(input_type const& msg) noexcept
        -> std::expected<void, phyriad::Error>
    {
        ++state_.msg_count;
        return outlet_.publish(msg);
    }

    // GraphRuntime entry point — reads one message from inlet and forwards it.
    // Returns RingEmpty if inlet has no message yet (non-fatal, caller yields).
    [[nodiscard]] auto tick() noexcept
        -> std::expected<void, phyriad::Error>
    {
        auto msg = inlet_.receive();
        if (!msg) return std::unexpected(msg.error());
        return on_message(*msg);
    }

    // ── Lifecycle hooks ───────────────────────────────────────────────────
    [[nodiscard]] auto on_start() noexcept
        -> std::expected<void, phyriad::Error>
    {
        state_.msg_count = 0;
        return {};
    }

    [[nodiscard]] auto on_stop() noexcept
        -> std::expected<void, phyriad::Error>
    {
        return {};
    }

    // ── Checkpointable interface ──────────────────────────────────────────
    struct State {
        uint64_t msg_count{0};
    };
    using state_type = State;

    [[nodiscard]] State const& state() const noexcept { return state_; }

    void restore_state(State const& snap) noexcept { state_ = snap; }

private:
    Outlet<output_type> outlet_;
    Inlet<input_type>   inlet_;
    State               state_{};
};

// ── Concept verification ──────────────────────────────────────────────────
static_assert(Source<CanonicalTransform>,
    "CanonicalTransform must satisfy Source");
static_assert(Sink<CanonicalTransform>,
    "CanonicalTransform must satisfy Sink");
static_assert(Transform<CanonicalTransform>,
    "CanonicalTransform must satisfy Transform");
static_assert(Stateful<CanonicalTransform>,
    "CanonicalTransform must satisfy Stateful (Checkpointable)");
static_assert(HasOnStart<CanonicalTransform>,
    "CanonicalTransform must satisfy HasOnStart");
static_assert(HasOnStop<CanonicalTransform>,
    "CanonicalTransform must satisfy HasOnStop");
static_assert(Node<CanonicalTransform>,
    "CanonicalTransform must satisfy Node");

} // namespace phyriad::node::canonical
// Made with my soul - Swately <3
