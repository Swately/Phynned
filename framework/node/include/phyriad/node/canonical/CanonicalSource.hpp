// framework/node/include/phyriad/node/canonical/CanonicalSource.hpp
// Canonical Source node — validates that Source<N> and lifecycle concepts
// are satisfiable with a concrete type (R3: zero stubs).
//
// CanonicalSource is NOT production code. It lives here to prove at compile
// time that the concept machinery works end-to-end. If any concept definition
// changes in a way that breaks conforming nodes, this file fails to compile.
//
// Satisfies: Source<CanonicalSource>, HasOnStart<CanonicalSource>, HasOnStop<CanonicalSource>
// Does NOT satisfy: Sink (no inlet), Actor (no tick)
//

#pragma once
#include "../Port.hpp"
#include "../Lifecycle.hpp"
#include "../Categories.hpp"
#include "../Node.hpp"
#include <phyriad/schema/PodMessage.hpp>
#include <expected>

namespace phyriad::node::canonical {

struct CanonicalSource {
    using output_type = schema::SampleTick;

    [[nodiscard]] Outlet<output_type>& outlet() noexcept { return outlet_; }

    // Lifecycle hooks — on_start initialises the sequence counter.
    [[nodiscard]] auto on_start() noexcept
        -> std::expected<void, phyriad::Error>
    {
        seq_ = 0;
        return {};
    }

    [[nodiscard]] auto on_stop() noexcept
        -> std::expected<void, phyriad::Error>
    {
        return {};
    }

    // Produce one tick and publish it.
    // Called by the scheduler loop or Actor::tick() in composite nodes.
    [[nodiscard]] auto emit() noexcept
        -> std::expected<void, phyriad::Error>
    {
        const output_type tick{
            .price    = seq_ * 100u,
            .volume   = 1u,
            .side     = 0u,
            .sequence = static_cast<uint32_t>(seq_),
        };
        ++seq_;
        return outlet_.publish(tick);
    }

    // GraphRuntime entry point — delegates to emit().
    [[nodiscard]] auto tick() noexcept
        -> std::expected<void, phyriad::Error>
    {
        return emit();
    }

private:
    Outlet<output_type> outlet_;
    uint64_t            seq_{0};
};

// ── Concept verification ──────────────────────────────────────────────────
static_assert(Source<CanonicalSource>,
    "CanonicalSource must satisfy Source<CanonicalSource>");
static_assert(HasOnStart<CanonicalSource>,
    "CanonicalSource must satisfy HasOnStart");
static_assert(HasOnStop<CanonicalSource>,
    "CanonicalSource must satisfy HasOnStop");
static_assert(Node<CanonicalSource>,
    "CanonicalSource must satisfy Node<CanonicalSource>");
static_assert(!Sink<CanonicalSource>,
    "CanonicalSource must NOT satisfy Sink (no inlet/on_message)");

} // namespace phyriad::node::canonical
// Made with my soul - Swately <3
