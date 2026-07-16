// framework/graph/include/phyriad/api/WireBuilder.hpp
// Fluent wire-connection builder returned by DslGraphBuilder::wire().
//
// Usage pattern:
//   builder.wire("md").to("fx")
//   builder.wire("fx").to("risk", WirePolicy::BlockUntilEvicted)
//
// WireBuilder is a transient object — do not store it. It holds a reference
// to the owning DslGraphBuilder and adds a WireEntry on .to().
//
// WireBuilder::to() body is defined in GraphDSL.hpp (after DslGraphBuilder is
// complete) to break the include cycle: DslGraphBuilder → WireBuilder →
// DslGraphBuilder. The declaration-before-definition pattern is standard C++.
//
// §3.H of PHASE_H_IMPLEMENTATION_PATTERNS.md
#pragma once
#include "NodeBuilder.hpp"   // WirePolicy
#include <string>
#include <string_view>

namespace phyriad::api {

class DslGraphBuilder;  // forward declaration — full definition in GraphDSL.hpp

// ── WireBuilder ───────────────────────────────────────────────────────────────
class WireBuilder {
public:
    WireBuilder(std::string_view from,
                uint32_t         outlet,
                DslGraphBuilder& builder) noexcept
        : from_{from}, from_outlet_{outlet}, builder_{&builder}
    {}

    // Non-copyable — transient, single-use chaining object.
    WireBuilder(WireBuilder const&)            = delete;
    WireBuilder& operator=(WireBuilder const&) = delete;
    WireBuilder(WireBuilder&&)                 = default;

    // Add a directed wire from `from_` to `dest` with the given policy.
    // Returns DslGraphBuilder& for continued chaining.
    // Body defined in GraphDSL.hpp where DslGraphBuilder is fully known.
    [[nodiscard]] DslGraphBuilder& to(
        std::string_view dest,
        uint32_t         inlet  = 0,
        WirePolicy       policy = WirePolicy::StrictWaitOrEvict) noexcept;

private:
    std::string       from_;
    uint32_t          from_outlet_{0};
    DslGraphBuilder*  builder_;
};

} // namespace phyriad::api
// Made with my soul - Swately <3
