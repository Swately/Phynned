// framework/node/include/phyriad/node/Port.hpp
// Type-erased Outlet<T> and Inlet<T> — connect nodes to transports.
//
// Outlet<T>: single-writer port. Type-erased via (SendFn, void*) pair.
//   Accepts any transport Tr satisfying: tr.send(T const&) noexcept -> expected<void,Error>.
//   Covers Latest<T>, Channel<T>, ShmTransportTyped<T,Id>, and Ring<T,Cap>.
//
// Inlet<T>: single-reader port. Type-erased via (RecvFn, DisconnectFn, void*) triple
//   plus an embedded RingHandle (only meaningful when connected to Ring<T,Cap>).
//   Subscribes on Ring::connect(), unsubscribes on disconnect()/destruction.
//
// connect() is called exactly once by GraphBuilder (Block E) at graph-build time.
// After wiring, publish()/receive() are the hot path — one indirect call each.
//
// Neither Outlet<T> nor Inlet<T> is copyable or movable:
//   Outlet — prevents accidental dual-writer scenarios (transports are SWMR).
//   Inlet  — owns a Ring subscription; address must be stable.
//

#pragma once
#include <phyriad/schema/Error.hpp>
#include <phyriad/schema/PodMessage.hpp>
#include <phyriad/transport/Ring.hpp>   // RingHandle + Ring<T,Cap>
#include <cstddef>
#include <cstdint>
#include <expected>

// Forward-declare GraphBuilder from Block E so connect() can be friended.
namespace phyriad::graph { template <typename> class GraphBuilder; }

namespace phyriad::node {

// ── Outlet<T> ──────────────────────────────────────────────────────────────
template <schema::PodMessage T>
class Outlet {
public:
    Outlet()  = default;
    ~Outlet() = default;

    Outlet(Outlet const&)            = delete;
    Outlet& operator=(Outlet const&) = delete;
    Outlet(Outlet&&)                 = delete;
    Outlet& operator=(Outlet&&)      = delete;

    [[nodiscard]] bool connected() const noexcept { return send_fn_ != nullptr; }

    // Runtime wiring — called by WireHandle::connect_outlet during GraphRuntime setup.
    // Accepts any transport whose send(T const&) noexcept returns expected<void,Error>.
    template <typename Tr>
        requires requires(Tr& tr, T const& m) {
            { tr.send(m) } noexcept -> std::same_as<std::expected<void, phyriad::Error>>;
        }
    void connect_runtime(Tr& tr) noexcept { connect(tr); }

    // Hot path — one indirect call through send_fn_.
    [[nodiscard]] auto publish(T const& msg) noexcept
        -> std::expected<void, phyriad::Error>
    {
        if (!connected()) [[unlikely]] {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::InvalidHandle,
                .source_node_id = 0,
                .timestamp_ns   = 0});
        }
        return send_fn_(ctx_, msg);
    }

private:
    using SendFn = std::expected<void, phyriad::Error>(*)(void*, T const&) noexcept;

    SendFn send_fn_{nullptr};
    void*  ctx_    {nullptr};

    // Wire Outlet to any transport whose send(T const&) noexcept returns expected<void,Error>.
    // Captureless lambda converts to function pointer with Tr baked into the instantiation.
    template <typename Tr>
        requires requires(Tr& tr, T const& m) {
            { tr.send(m) } noexcept
                -> std::same_as<std::expected<void, phyriad::Error>>;
        }
    void connect(Tr& tr) noexcept {
        disconnect();
        ctx_     = &tr;
        send_fn_ = [](void* ctx, T const& msg) noexcept
            -> std::expected<void, phyriad::Error>
        {
            return static_cast<Tr*>(ctx)->send(msg);
        };
    }

    void disconnect() noexcept {
        send_fn_ = nullptr;
        ctx_     = nullptr;
    }

    template <typename> friend class phyriad::graph::GraphBuilder;
};

// ── Inlet<T> ───────────────────────────────────────────────────────────────
template <schema::PodMessage T>
class Inlet {
public:
    Inlet()  = default;
    ~Inlet() noexcept { disconnect(); }

    Inlet(Inlet const&)            = delete;
    Inlet& operator=(Inlet const&) = delete;
    Inlet(Inlet&&)                 = delete;
    Inlet& operator=(Inlet&&)      = delete;

    [[nodiscard]] bool connected() const noexcept { return recv_fn_ != nullptr; }

    // Runtime wiring — called by WireHandle::connect_inlet during GraphRuntime setup.
    // For stateless transports (Latest<T>, Channel<T>, ShmTransportTyped).
    template <typename Tr>
        requires requires(Tr& tr) {
            { tr.receive() } noexcept -> std::same_as<std::expected<T, phyriad::Error>>;
        }
    void connect_runtime(Tr& tr) noexcept { connect(tr); }

    // Runtime wiring for Ring<T,Cap> — subscribes and embeds the handle.
    template <std::size_t Cap>
    void connect_ring_runtime(transport::Ring<T, Cap>& ring) noexcept { connect(ring); }

    // Hot path — one indirect call through recv_fn_.
    // RingHandle& is passed to the function for Ring inlets; non-Ring inlets ignore it.
    [[nodiscard]] auto receive() noexcept
        -> std::expected<T, phyriad::Error>
    {
        if (!connected()) [[unlikely]] {
            return std::unexpected(phyriad::Error{
                .code           = ErrorCode::InvalidHandle,
                .source_node_id = 0,
                .timestamp_ns   = 0});
        }
        return recv_fn_(ctx_, handle_);
    }

    // Explicit disconnect — releases Ring subscription. Safe to call multiple times.
    void disconnect() noexcept {
        if (disconnect_fn_) {
            disconnect_fn_(ctx_, handle_);
            disconnect_fn_ = nullptr;
        }
        recv_fn_ = nullptr;
        ctx_     = nullptr;
        handle_  = transport::RingHandle{};
    }

private:
    // RecvFn always receives RingHandle& — non-Ring impls ignore it.
    using RecvFn       = std::expected<T, phyriad::Error>(*)(void*, transport::RingHandle&) noexcept;
    using DisconnectFn = void(*)(void*, transport::RingHandle const&) noexcept;

    RecvFn               recv_fn_      {nullptr};
    DisconnectFn         disconnect_fn_{nullptr};
    void*                ctx_          {nullptr};
    transport::RingHandle handle_      {};

    // connect for transports with stateless receive() — Latest<T>, Channel<T>, ShmTransportTyped.
    // Disambiguated from Ring overload because Ring::receive() requires a RingHandle argument.
    template <typename Tr>
        requires requires(Tr& tr) {
            { tr.receive() } noexcept
                -> std::same_as<std::expected<T, phyriad::Error>>;
        }
    void connect(Tr& tr) noexcept {
        disconnect();
        ctx_           = &tr;
        disconnect_fn_ = nullptr;
        recv_fn_       = [](void* ctx, transport::RingHandle&) noexcept
            -> std::expected<T, phyriad::Error>
        {
            return static_cast<Tr*>(ctx)->receive();
        };
    }

    // connect for Ring<T,Cap>: subscribes and embeds handle.
    // On disconnect/destruction: unsubscribes via disconnect_fn_.
    template <std::size_t Cap>
    void connect(transport::Ring<T, Cap>& ring) noexcept {
        disconnect();
        transport::RingHandle h = ring.subscribe();
        if (!h.valid()) [[unlikely]] return;  // all 64 reader slots taken — stay disconnected

        handle_  = h;
        ctx_     = &ring;
        recv_fn_ = [](void* ctx, transport::RingHandle& h) noexcept
            -> std::expected<T, phyriad::Error>
        {
            return static_cast<transport::Ring<T, Cap>*>(ctx)->receive(h);
        };
        disconnect_fn_ = [](void* ctx, transport::RingHandle const& h) noexcept {
            static_cast<transport::Ring<T, Cap>*>(ctx)->unsubscribe(h);
        };
    }

    template <typename> friend class phyriad::graph::GraphBuilder;
};

} // namespace phyriad::node
// Made with my soul - Swately <3
