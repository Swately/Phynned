// framework/transport/include/phyriad/transport/Transport.hpp
// Transport concept and LatencyClass enum — the typed channel contract.
//
// Transport<Impl, Msg> is satisfied by channels that expose:
//   - type_hash        — compile-time schema fingerprint (schema::Hash128)
//   - latency_class()  — declared latency tier (noexcept)
//   - send(Msg const&) — noexcept, returns std::expected<void, phyriad::Error>
//   - receive()        — noexcept, returns std::expected<Msg,  phyriad::Error>
//
// Concrete types satisfying Transport:
//   Latest<T>   — in-process seqlock SWMR (LocalCache)
//   Ring<T,Cap> — in-process Disruptor-style SWMR (LocalCache, via RingHandle)
//

#pragma once
#include <phyriad/schema/Error.hpp>       // phyriad::Error
#include <phyriad/schema/PodMessage.hpp>  // schema::PodMessage, schema::Hash128
#include <concepts>
#include <cstdint>
#include <expected>

namespace phyriad::transport {

// ── LatencyClass — declared performance tier of a transport ──────────────────
enum class LatencyClass : uint8_t {
    LocalCache   = 0,  // same address space, same process — typically < 50 ns
    ProcessLocal = 1,  // cross-process same machine — typically < 500 ns
    CrossProcess = 2,  // SHM-backed cross-process — typically < 2 µs
};

// ── Transport concept ────────────────────────────────────────────────────────
// Requires the concrete type to advertise its schema hash and latency class
// so the graph builder can verify wire type compatibility at compile time.
template <typename Impl, typename Msg>
concept Transport =
    schema::PodMessage<Msg> &&
    requires(Impl& impl, Msg const& m) {
        { Impl::type_hash       } -> std::convertible_to<schema::Hash128>;
        { Impl::latency_class() } noexcept -> std::same_as<LatencyClass>;
        { impl.send(m)           } noexcept
            -> std::same_as<std::expected<void, phyriad::Error>>;
        { impl.receive()         } noexcept
            -> std::same_as<std::expected<Msg,  phyriad::Error>>;
    };

} // namespace phyriad::transport
// Made with my soul - Swately <3
