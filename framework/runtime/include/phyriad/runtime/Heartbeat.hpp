// framework/runtime/include/phyriad/runtime/Heartbeat.hpp
// Per-node heartbeat array — one cache-line-aligned atomic counter per node.
//
// HeartbeatSlot occupies exactly one cache line (hal::kDestructivePad).
// Each node's counter lives in an independent cache line to prevent false
// sharing when multiple node threads increment concurrently.
//
// GraphRuntime worker threads call heartbeats.tick(idx) at the end of every
// successful tick(). External monitors (orchestration::Watchdog, telemetry,
// liveness probes) read heartbeats.read(idx) to detect stalled nodes.
//
// Perf strategy:
//   - tick(): single `stat_fetch_add_relaxed` on a cache-line-isolated atomic.
//     ~5-15 ns on x86; no contention because each producer owns its slot.
//   - read(): `stat_load_relaxed` — same line, no synchronization needed.
//   - cursor(): exposes the atomic directly for spin-wait / busy-poll callers.
//

#pragma once
#include <phyriad/hal/Cacheline.hpp>
#include <phyriad/hal/MemoryOrder.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace phyriad::runtime {

// ── HeartbeatSlot ─────────────────────────────────────────────────────────────
// Each slot is exactly hal::kDestructivePad bytes — one destructive-pad line.
// The `counter` member is followed by explicit padding so the slot occupies
// the full cache line; std::vector<HeartbeatSlot> guarantees alignof preservation.
struct alignas(hal::kDestructivePad) HeartbeatSlot {
    std::atomic<uint64_t> counter{0};
    char _pad[hal::kDestructivePad - sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(HeartbeatSlot)  == hal::kDestructivePad,
    "HeartbeatSlot must be exactly hal::kDestructivePad bytes");
static_assert(alignof(HeartbeatSlot) == hal::kDestructivePad,
    "HeartbeatSlot alignment must equal hal::kDestructivePad");

// ── HeartbeatArray ────────────────────────────────────────────────────────────
class HeartbeatArray {
public:
    HeartbeatArray()                                  = default;
    explicit HeartbeatArray(std::size_t n) noexcept : slots_(n) {}
    ~HeartbeatArray()                                 = default;
    HeartbeatArray(HeartbeatArray const&)            = delete;
    HeartbeatArray& operator=(HeartbeatArray const&) = delete;
    HeartbeatArray(HeartbeatArray&&) noexcept        = default;
    HeartbeatArray& operator=(HeartbeatArray&&) noexcept = default;

    // Increment the counter for node at index `idx` (hot path — worker thread).
    void tick(std::size_t idx) noexcept {
        if (idx < slots_.size()) [[likely]]
            hal::stat_fetch_add_relaxed(slots_[idx].counter, uint64_t{1});
    }

    // Read the counter for node at index `idx` (cold path — Watchdog / diag).
    [[nodiscard]] uint64_t read(std::size_t idx) const noexcept {
        if (idx >= slots_.size()) return 0u;
        return hal::stat_load_relaxed(slots_[idx].counter);
    }

    // Direct atomic reference — Watchdog can spin-wait on this.
    [[nodiscard]] std::atomic<uint64_t>& cursor(std::size_t idx) noexcept {
        return slots_[idx].counter;
    }

    // Const-overload for read-only liveness probing (Watchdog PULL model).
    // `Watchdog::register_consumer` takes a `const atomic&` so
    // this overload lets callers wire the heartbeat → watchdog in one call:
    //   wd.register_consumer(node_id, hb.cursor_const(node_id), timeout_ms);
    [[nodiscard]] std::atomic<uint64_t> const& cursor_const(std::size_t idx) const noexcept {
        return slots_[idx].counter;
    }

    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

private:
    std::vector<HeartbeatSlot> slots_;
};

} // namespace phyriad::runtime
// Made with my soul - Swately <3
