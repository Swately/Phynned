// framework/graph/include/phyriad/api/WireHandle.hpp
// Type-erased ring-buffer wire handle for the GraphRuntime.
//
// WireHandle wraps a Ring<T, Cap> allocation and exposes three type-erased
// operations: connect_outlet, connect_inlet, destroy.
//
// Capacity dispatch: make<T>(cap) rounds cap up to the next power of 2 and
// selects from {64, 256, 1024, 4096}. The chosen capacity is stored in the
// `capacity` field for inspection.
//
// connect_outlet(void* outlet_ptr) — casts to Outlet<T>* and calls connect_runtime.
// connect_inlet(void* inlet_ptr)   — casts to Inlet<T>* and calls connect_ring_runtime.
//
// Ownership: WireHandle owns the ring allocation. Call destroy() exactly once
// before the handle goes out of scope (GraphRuntime handles this).
//
#pragma once
#include <phyriad/node/Port.hpp>
#include <phyriad/schema/SchemaHash.hpp>
#include <phyriad/schema/PodMessage.hpp>
#include <phyriad/transport/Ring.hpp>
#include <phyriad/hal/Cacheline.hpp>
#include <phyriad/hal/Allocator.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace phyriad::api {

namespace detail {

// Ring allocations are the largest in the framework
// (a Ring<Task, 65536> with 256B payload is 16 MiB). Large allocations
// benefit from page-allocator dedicated pages (no heap fragmentation) and,
// when SeLockMemoryPrivilege is available, hugepages (8 TLB entries for a
// 16 MiB buffer vs 4096 entries with 4 KiB pages).
//
// Threshold: 64 KiB. Below this, the standard malloc fast path is faster
// because VirtualAlloc/mmap has high per-call overhead. At/above 64 KiB,
// dedicated pages amortize.
inline constexpr std::size_t kRingLargeThreshold = 64u * 1024u;

[[nodiscard]] inline hal::AllocHint pick_ring_hint(std::size_t size) noexcept {
    if (size < kRingLargeThreshold) return hal::AllocHint::Default;
    hal::AllocHint h = hal::AllocHint::Large | hal::AllocHint::NumaLocal;
    if (size >= hal::k2MiB) h = h | hal::AllocHint::Huge;
    return h;
}

[[nodiscard]] inline void* aligned_alloc_wire(std::size_t size, std::size_t align) noexcept {
    return hal::aligned_alloc_hint(size, align, pick_ring_hint(size));
}

inline void aligned_free_wire(void* p, std::size_t size = 0u) noexcept {
    hal::aligned_free_hint(p, size, pick_ring_hint(size));
}

[[nodiscard]] inline constexpr uint32_t next_pow2_wire(uint32_t n) noexcept {
    if (n == 0u) return 1u;
    n--;
    n |= n >> 1u;
    n |= n >> 2u;
    n |= n >> 4u;
    n |= n >> 8u;
    n |= n >> 16u;
    return n + 1u;
}

} // namespace detail

// ── WireHandle ────────────────────────────────────────────────────────────────
struct alignas(hal::kDestructivePad) WireHandle {
    void*            ring{nullptr};
    schema::Hash128  type_hash{};
    std::uint32_t    wire_id{0};
    std::uint32_t    src_node_id{0};
    std::uint32_t    dst_node_id{0};
    std::uint32_t    capacity{0};

    void (*destroy_fn)       (void*)              noexcept{nullptr};
    void (*connect_outlet_fn)(void*, void* outlet) noexcept{nullptr};
    void (*connect_inlet_fn) (void*, void* inlet)  noexcept{nullptr};

    // ── Validity ──────────────────────────────────────────────────────────────
    [[nodiscard]] bool valid() const noexcept { return ring != nullptr; }

    void connect_outlet(void* outlet_ptr) noexcept {
        if (connect_outlet_fn && outlet_ptr) connect_outlet_fn(ring, outlet_ptr);
    }
    void connect_inlet(void* inlet_ptr) noexcept {
        if (connect_inlet_fn && inlet_ptr) connect_inlet_fn(ring, inlet_ptr);
    }
    void destroy() noexcept {
        if (destroy_fn && ring) {
            destroy_fn(ring);
            ring = nullptr;
        }
    }

    // ── Factory ───────────────────────────────────────────────────────────────
    // Selects the smallest Ring<T, Cap> where Cap >= capacity (rounded to pow2).
    // Caps at Ring<T, 4096>.
    template <schema::PodMessage T>
    [[nodiscard]] static WireHandle make(uint32_t wire_id, uint32_t src,
                                         uint32_t dst, uint32_t capacity) noexcept {
        uint32_t cap = std::max(64u, detail::next_pow2_wire(capacity));
        if (cap <= 64u)   return make_ring_<T, 64u>  (wire_id, src, dst, cap);
        if (cap <= 256u)  return make_ring_<T, 256u> (wire_id, src, dst, cap);
        if (cap <= 1024u) return make_ring_<T, 1024u>(wire_id, src, dst, cap);
        return             make_ring_<T, 4096u>       (wire_id, src, dst, cap);
    }

private:
    template <schema::PodMessage T, std::size_t Cap>
    [[nodiscard]] static WireHandle make_ring_(uint32_t wire_id, uint32_t src,
                                               uint32_t dst, uint32_t cap) noexcept {
        using RingT = transport::Ring<T, Cap>;
        void* mem = detail::aligned_alloc_wire(sizeof(RingT), alignof(RingT));
        if (!mem) return {};
        RingT* rp = new(mem) RingT{};

        WireHandle h{};
        h.ring          = rp;
        h.type_hash     = schema::schema_hash<T>();
        h.wire_id       = wire_id;
        h.src_node_id   = src;
        h.dst_node_id   = dst;
        h.capacity      = cap;

        h.destroy_fn = [](void* r) noexcept {
            auto* ring = static_cast<RingT*>(r);
            ring->~RingT();
            // Pass sizeof(RingT) to free — required by the OS-page free path
            // (POSIX munmap needs size; Windows VirtualFree ignores it).
            detail::aligned_free_wire(ring, sizeof(RingT));
        };

        h.connect_outlet_fn = [](void* r, void* outlet_ptr) noexcept {
            auto* ring   = static_cast<RingT*>(r);
            auto* outlet = static_cast<node::Outlet<T>*>(outlet_ptr);
            outlet->connect_runtime(*ring);
        };

        h.connect_inlet_fn = [](void* r, void* inlet_ptr) noexcept {
            auto* ring  = static_cast<RingT*>(r);
            auto* inlet = static_cast<node::Inlet<T>*>(inlet_ptr);
            inlet->connect_ring_runtime(*ring);
        };

        return h;
    }
};

static_assert(alignof(WireHandle) == hal::kDestructivePad,
    "WireHandle must be aligned to hal::kDestructivePad");

} // namespace phyriad::api
// Made with my soul - Swately <3
