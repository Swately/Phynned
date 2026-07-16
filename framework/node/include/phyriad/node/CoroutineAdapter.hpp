// framework/node/include/phyriad/node/CoroutineAdapter.hpp
// Coroutine frame slab allocator and AwaitableTask<T>.
//
// CoroFrameAllocator: per-thread slab that satisfies coroutine frame
//   allocation without hitting the system allocator on the hot path.
//
//   Layout: pool_ = kPoolSize × kFrameSize contiguous bytes (256 × 1024 = 256 KB).
//   Tracking: free_bitmap_ — 4 × atomic<uint64_t> covering 256 slots (1 bit = 1 slot).
//   Init: all bits set (all frames free).
//
//   allocate(n):
//     n <= kFrameSize — scan bitmap words for a free slot, CAS to claim, return ptr.
//     n >  kFrameSize — fallback to ::operator new (oversized frame, rare).
//     slab exhausted  — fallback to ::operator new.
//
//   deallocate(p, n):
//     p in pool range → restore bit; else → ::operator delete.
//
//   Thread model: thread_local instance — each thread owns its slab exclusively.
//   Cross-thread deallocation is safe (bitmap CAS), but rare in Phyriad (nodes
//   are pinned; coroutines complete on the same thread they started on).
//
// AwaitableTask<T>: coroutine return type that uses CoroFrameAllocator.
//   Stores the result value in promise_type.
//   promise_type::operator new/delete route through CoroFrameAllocator::instance().
//

#pragma once
#include <phyriad/hal/Cacheline.hpp>
#include <phyriad/schema/Error.hpp>
#include <array>
#include <atomic>
#include <bit>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <utility>
#include <phyriad/hal/MemoryOrder.hpp>

namespace phyriad::node {

// ── CoroFrameAllocator ────────────────────────────────────────────────────
class CoroFrameAllocator {
public:
    static constexpr std::size_t kFrameSize  = 1024;
    static constexpr std::size_t kPoolSize   = 256;
    static constexpr std::size_t kBitmapWords = kPoolSize / 64;  // 4

    CoroFrameAllocator() noexcept {
        for (auto& w : free_bitmap_) {
            hal::stat_store_relaxed(w, ~uint64_t{0});  // single-thread init
        }
    }

    ~CoroFrameAllocator() = default;
    CoroFrameAllocator(CoroFrameAllocator const&) = delete;
    CoroFrameAllocator& operator=(CoroFrameAllocator const&) = delete;

    // Returns the thread-local instance.
    // Thread-local guarantees each scheduler thread gets its own slab
    // with zero cross-thread contention on the allocation fast path.
    [[nodiscard]] static CoroFrameAllocator& instance() noexcept {
        thread_local CoroFrameAllocator inst{};
        return inst;
    }

    [[nodiscard]] void* allocate(std::size_t n) noexcept {
        if (n > kFrameSize) [[unlikely]] {
            return ::operator new(n, std::nothrow);
        }
        for (std::size_t w = 0; w < kBitmapWords; ++w) {
            uint64_t cur = hal::seq_load_acquire(free_bitmap_[w]);
            while (cur != 0) {
                const uint32_t bit     = static_cast<uint32_t>(std::countr_zero(cur));
                const uint64_t desired = cur & ~(uint64_t{1} << bit);
                if (free_bitmap_[w].compare_exchange_weak(
                        cur, desired,
                        std::memory_order_acq_rel,  // HAL: explicit ordering — see surrounding context
                        std::memory_order_relaxed)) {  // HAL: relaxed — secondary atomic in compound op
                    const std::size_t slot = w * 64u + bit;
                    return pool_.data() + slot * kFrameSize;
                }
                // cur refreshed by CAS failure; retry inner loop
            }
        }
        return ::operator new(n, std::nothrow);  // slab exhausted — fallback
    }

    void deallocate(void* p, std::size_t n) noexcept {
        if (n > kFrameSize) [[unlikely]] {
            ::operator delete(p, n);
            return;
        }
        auto* const base = pool_.data();
        auto* const ptr  = static_cast<std::byte*>(p);
        if (ptr < base || ptr >= base + kPoolSize * kFrameSize) [[unlikely]] {
            // Frame came from system allocator (slab was exhausted at alloc time).
            ::operator delete(p, n);
            return;
        }
        const std::size_t slot = static_cast<std::size_t>(ptr - base) / kFrameSize;
        const std::size_t w    = slot / 64u;
        const uint32_t    bit  = static_cast<uint32_t>(slot % 64u);
        free_bitmap_[w].fetch_or(
            uint64_t{1} << bit,
            std::memory_order_release);  // HAL: release ordering — CAS success or paired ack
    }

private:
    // Pool memory: 256 × 1024 = 256 KB per thread. Aligned to kDestructivePad
    // so the first slot starts on a cache-line boundary (avoids false sharing
    // between adjacent slots that straddle a cacheline on first access).
    alignas(hal::kDestructivePad)
        std::array<std::byte, kFrameSize * kPoolSize> pool_{};

    // Bitmap: 4 × 64-bit words → 256 slots. Bit=1 means slot is FREE.
    // No alignment padding needed — 4 atomics fit comfortably in 32 bytes,
    // no false sharing with pool_ due to separate cache lines.
    std::array<std::atomic<uint64_t>, kBitmapWords> free_bitmap_{};
};

// ── AwaitableTask<T> ──────────────────────────────────────────────────────
// Coroutine return type whose frames are allocated from CoroFrameAllocator.
// Usage:
//   AwaitableTask<int> my_coro() {
//       co_return 42;
//   }
//   auto task = my_coro();
//   task.handle().resume();
//   int result = task.value();
template <typename T>
struct AwaitableTask {
    struct promise_type {
        T value{};

        // Route frame allocation through the thread-local slab.
        [[nodiscard]] static void* operator new(std::size_t n) noexcept {
            return CoroFrameAllocator::instance().allocate(n);
        }
        static void operator delete(void* p, std::size_t n) noexcept {
            CoroFrameAllocator::instance().deallocate(p, n);
        }

        // Required when operator new is noexcept: return null-handle on OOM.
        [[nodiscard]] static AwaitableTask get_return_object_on_allocation_failure() noexcept {
            return AwaitableTask{nullptr};
        }

        [[nodiscard]] AwaitableTask get_return_object() noexcept {
            return AwaitableTask{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // Suspend immediately — caller resumes explicitly via handle().resume().
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }

        // Suspend at final point — caller must destroy the handle after reading value.
        [[nodiscard]] std::suspend_always final_suspend() noexcept { return {}; }

        void return_value(T v) noexcept { value = std::move(v); }

        // Unhandled exceptions in a noexcept coroutine body are UB.
        // Terminate explicitly: the DataflowFn concept enforces noexcept.
        [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit AwaitableTask(handle_type h) noexcept : handle_(h) {}

    ~AwaitableTask() noexcept {
        if (handle_) handle_.destroy();
    }

    // Non-copyable — one owner, one handle.
    AwaitableTask(AwaitableTask const&) = delete;
    AwaitableTask& operator=(AwaitableTask const&) = delete;

    // Movable: transfer handle ownership.
    AwaitableTask(AwaitableTask&& o) noexcept
        : handle_(std::exchange(o.handle_, {})) {}
    AwaitableTask& operator=(AwaitableTask&&) = delete;

    [[nodiscard]] handle_type handle() noexcept { return handle_; }

    // Access the return value — call only after handle().done() == true.
    [[nodiscard]] T const& value() const noexcept { return handle_.promise().value; }

private:
    handle_type handle_{};
};

// ── AwaitableTask<void> specialization ───────────────────────────────────
template <>
struct AwaitableTask<void> {
    struct promise_type {
        [[nodiscard]] static void* operator new(std::size_t n) noexcept {
            return CoroFrameAllocator::instance().allocate(n);
        }
        static void operator delete(void* p, std::size_t n) noexcept {
            CoroFrameAllocator::instance().deallocate(p, n);
        }

        // Required when operator new is noexcept: return null-handle on OOM.
        [[nodiscard]] static AwaitableTask get_return_object_on_allocation_failure() noexcept {
            return AwaitableTask{nullptr};
        }

        [[nodiscard]] AwaitableTask get_return_object() noexcept {
            return AwaitableTask{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend()   noexcept { return {}; }
        void return_void() noexcept {}
        [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit AwaitableTask(handle_type h) noexcept : handle_(h) {}
    ~AwaitableTask() noexcept { if (handle_) handle_.destroy(); }

    AwaitableTask(AwaitableTask const&) = delete;
    AwaitableTask& operator=(AwaitableTask const&) = delete;
    AwaitableTask(AwaitableTask&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}
    AwaitableTask& operator=(AwaitableTask&&) = delete;

    [[nodiscard]] handle_type handle() noexcept { return handle_; }

private:
    handle_type handle_{};
};

} // namespace phyriad::node
// Made with my soul - Swately <3
