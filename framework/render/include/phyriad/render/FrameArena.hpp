// framework/render/include/phyriad/render/FrameArena.hpp
// Per-frame bump allocator for ImGui draw-list memory.
//
// FrameArena owns a single contiguous aligned buffer. Allocations advance a
// bump pointer; there is no individual free — memory is reclaimed wholesale
// via reset_frame() at the start of each frame.
//
// Thread safety: alloc() uses a CAS loop on offset_ — safe for concurrent use
// (e.g. background worker populating an overlay while RenderNode submits draw
// commands). reset_frame() is NOT thread-safe: caller must ensure no concurrent
// alloc() is in flight when reset_frame() is called (guaranteed by the frame
// boundary protocol).
//
// Budget: the capacity is fixed at construction. If an allocation exceeds the
// remaining budget, alloc() returns nullptr. There is NO fallback malloc.
// Callers must gracefully handle nullptr (skip the allocation, log, or throttle).
//
// ImGui integration:
//   FrameArena arena;
//   ImGui::SetAllocatorFunctions(
//       [](size_t sz, void* ud) { return static_cast<FrameArena*>(ud)->alloc(sz); },
//       [](void*,     void*)    {                                                 },
//       &arena);
//   ImGui::CreateContext();
//

#pragma once
#include <phyriad/hal/Allocator.hpp>
#include <phyriad/hal/MemoryOrder.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace phyriad::render {

class FrameArena {
public:
    // ── Construction ─────────────────────────────────────────────────────────
    explicit FrameArena(std::size_t capacity = 4u * 1024u * 1024u) noexcept
        : capacity_(capacity)
    {
        base_ = static_cast<std::byte*>(aligned_alloc_(capacity, 64u));
        if (!base_) capacity_ = 0u;
    }

    ~FrameArena() noexcept {
        if (base_) { aligned_free_(base_, capacity_); base_ = nullptr; }
    }

    FrameArena(FrameArena const&)            = delete;
    FrameArena& operator=(FrameArena const&) = delete;

    FrameArena(FrameArena&& o) noexcept
        : base_      (o.base_)
        , capacity_  (o.capacity_)
        , offset_    (phyriad::hal::stat_load_relaxed(o.offset_))
        , high_water_(phyriad::hal::stat_load_relaxed(o.high_water_))
    {
        o.base_     = nullptr;
        o.capacity_ = 0u;
        phyriad::hal::stat_store_relaxed(o.offset_,     std::size_t{0u});
        phyriad::hal::stat_store_relaxed(o.high_water_, std::size_t{0u});
    }

    // ── alloc ─────────────────────────────────────────────────────────────────
    [[nodiscard]] void* alloc(std::size_t size, std::size_t align = 16u) noexcept {
        if (!base_) [[unlikely]] return nullptr;
        if (align == 0u) align = 1u;

        std::size_t cur = phyriad::hal::stat_load_relaxed(offset_);
        for (;;) {
            const std::size_t aligned_offset = (cur + align - 1u) & ~(align - 1u);
            const std::size_t end = aligned_offset + size;
            if (end > capacity_) [[unlikely]] return nullptr;

            if (offset_.compare_exchange_weak(cur, end,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {  // HAL: relaxed — secondary atomic in compound op
                std::size_t hw = phyriad::hal::stat_load_relaxed(high_water_);
                while (end > hw &&
                       !high_water_.compare_exchange_weak(hw, end,
                           std::memory_order_relaxed, std::memory_order_relaxed)) {}  // HAL: relaxed — secondary atomic in compound op
                return base_ + aligned_offset;
            }
        }
    }

    void free(void* /*ptr*/) noexcept {}

    void reset_frame() noexcept {
        phyriad::hal::stat_store_relaxed(offset_, std::size_t{0u});
    }

    [[nodiscard]] std::size_t bytes_used()      const noexcept { return phyriad::hal::stat_load_relaxed(offset_); }
    [[nodiscard]] std::size_t capacity()        const noexcept { return capacity_; }
    [[nodiscard]] std::size_t high_water_mark() const noexcept { return phyriad::hal::stat_load_relaxed(high_water_); }
    [[nodiscard]] bool        valid()           const noexcept { return base_ != nullptr; }

private:
    // FrameArena's default capacity is 4 MiB (ImGui draw-list
    // memory). At that size we benefit from the OS page allocator (no heap
    // fragmentation, dedicated TLB entries with hugepages when available).
    // Below 64 KiB use the standard fast-path malloc.
    [[nodiscard]] static hal::AllocHint pick_hint_(std::size_t size) noexcept {
        if (size < 64u * 1024u) return hal::AllocHint::Default;
        hal::AllocHint h = hal::AllocHint::Large | hal::AllocHint::NumaLocal;
        if (size >= hal::k2MiB) h = h | hal::AllocHint::Huge;
        return h;
    }

    [[nodiscard]] static void* aligned_alloc_(std::size_t size, std::size_t align) noexcept {
        return hal::aligned_alloc_hint(size, align, pick_hint_(size));
    }

    static void aligned_free_(void* p, std::size_t size) noexcept {
        hal::aligned_free_hint(p, size, pick_hint_(size));
    }

    std::byte*               base_      {nullptr};
    std::size_t              capacity_  {0u};
    std::atomic<std::size_t> offset_    {0u};
    std::atomic<std::size_t> high_water_{0u};
};

} // namespace phyriad::render
// Made with my soul - Swately <3
