// framework/graph/include/phyriad/api/NodeHandle.hpp
// Type-erased, 64B-aligned node handle for the GraphRuntime.
//
// NodeHandle stores:
//   - void* state       — 64B-aligned heap allocation for the node instance
//   - Function pointers — tick, start, stop, destroy, outlet_at, inlet_at
//   - Metadata          — node_id, type hashes, port counts
//
// All function pointers are captureless lambdas converted to raw function pointers
// at NodeHandle::make<N>() time — zero overhead on the hot tick path.
//
// Memory model:
//   - state is allocated with alignas(hal::kDestructivePad) (64B)
//   - The NodeHandle struct itself is alignas(hal::kDestructivePad)
//   - Move invalidates the moved-from handle (state = nullptr)
//   - Copyable as a struct (but copying raw pointers — owner tracks lifecycle)
//
#pragma once
#include <phyriad/node/Node.hpp>
#include <phyriad/node/Runnable.hpp>
#include <phyriad/node/Lifecycle.hpp>
#include <phyriad/node/Categories.hpp>
#include <phyriad/schema/SchemaHash.hpp>
#include <phyriad/hal/Cacheline.hpp>
#include <phyriad/schema/Error.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <new>
#include <type_traits>

#ifdef _WIN32
#  include <malloc.h>
#endif

namespace phyriad::api {

// ── Aligned alloc helpers ─────────────────────────────────────────────────────
namespace detail {

[[nodiscard]] inline void* aligned_alloc_node(std::size_t size, std::size_t align) noexcept {
#ifdef _WIN32
    return _aligned_malloc(size, align);
#else
    // C11 std::aligned_alloc requires size to be a multiple of align —
    // calling with mismatched values (e.g. align=128, size=24) is UB
    // and AddressSanitizer reports it as
    //   "invalid alignment requested in aligned_alloc: 128, alignment
    //    must be a power of two and the requested size 0x18 must be
    //    a multiple of alignment".
    // Round size up to the next multiple of align before delegating.
    // The trailing bytes are wasted but the caller-visible useable
    // region is still `size`.
    const std::size_t rounded = (size + align - 1u) & ~(align - 1u);
    return std::aligned_alloc(align, rounded);
#endif
}

inline void aligned_free_node(void* p) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace detail

// ── NodeHandle ────────────────────────────────────────────────────────────────
struct alignas(hal::kDestructivePad) NodeHandle {
    void*            state{nullptr};
    NodeId           node_id{0};
    schema::Hash128  output_type_hash{};
    schema::Hash128  input_type_hash{};
    std::uint32_t    outlet_count{0};
    std::uint32_t    inlet_count{0};

    std::expected<void, phyriad::Error> (*tick_fn)   (void*) noexcept {nullptr};
    void                              (*start_fn)  (void*) noexcept {nullptr};
    void                              (*stop_fn)   (void*) noexcept {nullptr};
    void                              (*destroy_fn)(void*) noexcept {nullptr};
    void*                             (*outlet_at_fn)(void*, std::size_t) noexcept {nullptr};
    void*                             (*inlet_at_fn) (void*, std::size_t) noexcept {nullptr};

    // ── Validity ──────────────────────────────────────────────────────────────
    [[nodiscard]] bool valid() const noexcept {
        return state != nullptr && tick_fn != nullptr;
    }

    // ── Hot-path wrappers ─────────────────────────────────────────────────────
    [[nodiscard]] std::expected<void, phyriad::Error> tick() noexcept {
        return tick_fn(state);
    }

    void start() noexcept { if (start_fn) start_fn(state); }
    void stop()  noexcept { if (stop_fn)  stop_fn(state); }

    void destroy() noexcept {
        if (destroy_fn && state) {
            destroy_fn(state);
            state = nullptr;
        }
    }

    // Returns void* to Outlet<N::output_type> for slot i (nullptr if out of range).
    [[nodiscard]] void* outlet_at(std::size_t i) noexcept {
        if (!outlet_at_fn) return nullptr;
        return outlet_at_fn(state, i);
    }

    // Returns void* to Inlet<N::input_type> for slot i (nullptr if out of range).
    [[nodiscard]] void* inlet_at(std::size_t i) noexcept {
        if (!inlet_at_fn) return nullptr;
        return inlet_at_fn(state, i);
    }

    // ── Factory ───────────────────────────────────────────────────────────────
    // Allocates node N with 64B alignment, constructs it in place, and wires
    // all function pointers using captureless lambdas → zero overhead at call time.
    template <Runnable N>
    [[nodiscard]] static NodeHandle make(NodeId id) noexcept {
        void* mem = detail::aligned_alloc_node(sizeof(N), hal::kDestructivePad);
        if (!mem) return {};
        N* node = new(mem) N{};

        NodeHandle h{};
        h.state   = node;
        h.node_id = id;

        // ── Type hashes ───────────────────────────────────────────────────────
        if constexpr (node::Source<N>) {
            h.output_type_hash = schema::schema_hash<typename N::output_type>();
            h.outlet_count = 1u;
        }
        // Detect inlet port: full Sink<N> (has on_message) or Actor-style node
        // that declares input_type + inlet() but drives receive() from tick() directly.
        if constexpr (node::Sink<N> ||
                      (requires { typename N::input_type; } &&
                       requires(N& n) { n.inlet(); })) {
            h.input_type_hash = schema::schema_hash<typename N::input_type>();
            h.inlet_count = 1u;
        }

        // ── tick ──────────────────────────────────────────────────────────────
        h.tick_fn = [](void* s) noexcept -> std::expected<void, phyriad::Error> {
            return static_cast<N*>(s)->tick();
        };

        // ── start / stop ──────────────────────────────────────────────────────
        h.start_fn = [](void* s) noexcept {
            (void)node::try_start(*static_cast<N*>(s));
        };
        h.stop_fn = [](void* s) noexcept {
            (void)node::try_stop(*static_cast<N*>(s));
        };

        // ── destroy ───────────────────────────────────────────────────────────
        h.destroy_fn = [](void* s) noexcept {
            N* n = static_cast<N*>(s);
            n->~N();
            detail::aligned_free_node(s);
        };

        // outlet_at
        if constexpr (requires(N& n, std::size_t i) {
                        { n.outlet_at_erased(i) } -> std::same_as<void*>; }) {
            h.outlet_at_fn = [](void* s, std::size_t i) noexcept -> void* {
                return static_cast<N*>(s)->outlet_at_erased(i);
            };
        } else if constexpr (node::Source<N>) {
            h.outlet_at_fn = [](void* s, std::size_t i) noexcept -> void* {
                if (i != 0u) return nullptr;
                return &static_cast<N*>(s)->outlet();
            };
        }

        // inlet_at
        if constexpr (requires(N& n, std::size_t i) {
                        { n.inlet_at_erased(i) } -> std::same_as<void*>; }) {
            h.inlet_at_fn = [](void* s, std::size_t i) noexcept -> void* {
                return static_cast<N*>(s)->inlet_at_erased(i);
            };
        } else if constexpr (node::Sink<N> ||
                            (requires { typename N::input_type; } &&
                            requires(N& n) { n.inlet(); })) {
            h.inlet_at_fn = [](void* s, std::size_t i) noexcept -> void* {
                if (i != 0u) return nullptr;
                return &static_cast<N*>(s)->inlet();
            };
        }

        return h;
    }

    // ── Non-owning wrapper ────────────────────────────────────────────────────
    // Create a NodeHandle pointing to an externally-managed node instance.
    // The node must outlive this handle. destroy() is a no-op.
    // Use when the node is not default-constructible (e.g., UIThreadNode,
    // RenderNode<States...>) and ownership is retained by the caller.
    // Uses WrappableNode<N> instead of Runnable<N> to allow non-default-
    // constructible nodes (Runnable additionally requires default_constructible).
    template <WrappableNode N>
    [[nodiscard]] static NodeHandle wrap(N* node_ptr, NodeId id) noexcept {
        if (!node_ptr) return {};

        NodeHandle h{};
        h.state   = node_ptr;
        h.node_id = id;

        // ── Type hashes (same as make<N>) ─────────────────────────────────────
        if constexpr (node::Source<N>) {
            h.output_type_hash = schema::schema_hash<typename N::output_type>();
            h.outlet_count = 1u;
        }
        if constexpr (node::Sink<N> ||
                      (requires { typename N::input_type; } &&
                       requires(N& n) { n.inlet(); })) {
            h.input_type_hash = schema::schema_hash<typename N::input_type>();
            h.inlet_count = 1u;
        }

        // ── tick / start / stop (same as make<N>) ─────────────────────────────
        h.tick_fn = [](void* s) noexcept -> std::expected<void, phyriad::Error> {
            return static_cast<N*>(s)->tick();
        };
        h.start_fn = [](void* s) noexcept {
            (void)node::try_start(*static_cast<N*>(s));
        };
        h.stop_fn = [](void* s) noexcept {
            (void)node::try_stop(*static_cast<N*>(s));
        };

        // ── destroy — no-op: caller retains ownership ─────────────────────────
        h.destroy_fn = [](void* /*s*/) noexcept {};

        // ── outlet_at ─────────────────────────────────────────────────────────
        if constexpr (requires(N& n, std::size_t i) {
                        { n.outlet_at_erased(i) } -> std::same_as<void*>; }) {
            h.outlet_at_fn = [](void* s, std::size_t i) noexcept -> void* {
                return static_cast<N*>(s)->outlet_at_erased(i);
            };
        } else if constexpr (node::Source<N>) {
            h.outlet_at_fn = [](void* s, std::size_t i) noexcept -> void* {
                if (i != 0u) return nullptr;
                return &static_cast<N*>(s)->outlet();
            };
        }

        // ── inlet_at ──────────────────────────────────────────────────────────
        if constexpr (requires(N& n, std::size_t i) {
                        { n.inlet_at_erased(i) } -> std::same_as<void*>; }) {
            h.inlet_at_fn = [](void* s, std::size_t i) noexcept -> void* {
                return static_cast<N*>(s)->inlet_at_erased(i);
            };
        } else if constexpr (node::Sink<N> ||
                            (requires { typename N::input_type; } &&
                            requires(N& n) { n.inlet(); })) {
            h.inlet_at_fn = [](void* s, std::size_t i) noexcept -> void* {
                if (i != 0u) return nullptr;
                return &static_cast<N*>(s)->inlet();
            };
        }

        return h;
    }
};

// NodeHandle is 64B-aligned; static_assert alignment but not size (varies by platform pointer size).
static_assert(alignof(NodeHandle) == hal::kDestructivePad,
    "NodeHandle must be aligned to hal::kDestructivePad");

} // namespace phyriad::api
// Made with my soul - Swately <3
