// framework/transport/include/phyriad/transport/Ring.hpp
// In-process typed ring buffer — SWMR (single-writer, multi-reader).
//
// Design: Disruptor-inspired SWMR. Owns its slot storage (std::array<T,Cap>).
// Use for same-process, same-NUMA low-latency message passing between nodes.
//
// Cache-line layout:
//   ProducerSide          alignas(kDestructivePad) — cursor + live_readers_bitmap
//   ReaderState[64]       alignas(kDestructivePad) — 64 × one cache line each
//   std::array<T, Cap>    alignas(kDestructivePad) — slot storage
//   next_producer_seq_    (producer-local, not shared)
//   cached_min_cursor_    (producer-local, not shared)
//
// Memory ordering:
//   Producer: write slots_[idx], then seq_store_release(cursor, idx).
//   Consumer: seq_load_acquire(cursor) establishes happens-before with slot write.
//   Subscribe/unsubscribe: acq_rel CAS on status + acq_rel on live_readers_bitmap.
//
// NOTE: Ring<T,Cap> does NOT directly satisfy Transport<Impl,Msg> — its receive()
// requires an explicit RingHandle (per-reader state). The Node layer wraps Ring in
// a Transport-compatible Outlet/Inlet with embedded handles.
//
// Capacity constraints: must be a power of 2 (bitmask index wrap).
//

#pragma once
#include "Transport.hpp"
#include <phyriad/schema/Error.hpp>
#include <phyriad/schema/PodMessage.hpp>
#include <phyriad/hal/Cacheline.hpp>
#include <phyriad/hal/MemoryOrder.hpp>
#include <phyriad/hal/Prefetch.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

namespace phyriad::transport {

inline constexpr uint32_t kMaxRingReaders = 64;

// ── Internal cache-line-padded structs ───────────────────────────────────────
namespace ring_detail {

inline constexpr uint32_t kFreeReader = 0;
inline constexpr uint32_t kLiveReader = 1;

// ProducerSide: cursor published by writer + live-reader bitmap for backpressure.
// Both fields in same cache line: the producer reads both in the hot send() path.
struct alignas(hal::kDestructivePad) ProducerSide {
    std::atomic<int64_t>  cursor{-1};
    std::atomic<uint64_t> live_readers_bitmap{0};
    char _pad[hal::kDestructivePad
              - sizeof(std::atomic<int64_t>)
              - sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(ProducerSide) == hal::kDestructivePad,
    "ProducerSide must occupy exactly one destructive-pad line");

// ReaderState: one struct per reader, each in its own cache line.
// cursor         — last consumed seq; producer reads for backpressure.
// heartbeat_seq  — bumped by reader; watchdog checks liveness.
// status         — FREE/LIVE; determines which readers are counted in bitmap.
struct alignas(hal::kDestructivePad) ReaderState {
    std::atomic<uint32_t> status{kFreeReader};
    char                  _pad0[4];
    std::atomic<int64_t>  cursor{-1};
    std::atomic<uint64_t> heartbeat_seq{0};
    char _pad1[hal::kDestructivePad - 4 - 4 - 8 - 8];
};
static_assert(sizeof(ReaderState) == hal::kDestructivePad,
    "ReaderState must occupy exactly one destructive-pad line");

} // namespace ring_detail

// ── RingHandle — caller-managed per-reader state ─────────────────────────────
// Caller creates a handle via Ring::subscribe(), passes it to Ring::receive().
// next_seq is local to the reader — no shared memory, no false sharing.
struct RingHandle {
    uint32_t reader_id{UINT32_MAX};
    int64_t  next_seq{0};
    [[nodiscard]] constexpr bool valid() const noexcept {
        return reader_id != UINT32_MAX;
    }
};

// ── Ring<T, Cap> ─────────────────────────────────────────────────────────────
template <schema::PodMessage T, std::size_t Cap>
    requires (Cap > 0 && (Cap & (Cap - 1)) == 0)  // must be power of 2
class Ring {
public:
    static constexpr schema::Hash128 type_hash = schema::schema_hash<T>();

    static constexpr LatencyClass latency_class() noexcept {
        return LatencyClass::LocalCache;
    }

    Ring()  = default;
    ~Ring() = default;

    // Not copyable or movable — contains atomics; must be pinned in memory.
    Ring(Ring const&)            = delete;
    Ring& operator=(Ring const&) = delete;
    Ring(Ring&&)                 = delete;
    Ring& operator=(Ring&&)      = delete;

    // ── subscribe ─────────────────────────────────────────────────────────────
    // Claim a reader slot and return a handle. Returns invalid handle if all
    // kMaxRingReaders slots are taken. The new reader starts from the NEXT
    // message published after this call (not from the beginning of the ring).
    [[nodiscard]] RingHandle subscribe() noexcept {
        for (uint32_t i = 0; i < kMaxRingReaders; ++i) {
            uint32_t expected = ring_detail::kFreeReader;
            if (readers_[i].status.compare_exchange_strong(
                    expected, ring_detail::kLiveReader,
                    std::memory_order_acq_rel)) {  // HAL: explicit ordering — see surrounding context
                const int64_t pc = hal::seq_load_acquire(producer_.cursor);
                hal::seq_store_release(readers_[i].cursor, pc);
                producer_.live_readers_bitmap.fetch_or(
                    1ULL << i, std::memory_order_acq_rel);  // HAL: explicit ordering — see surrounding context
                return RingHandle{i, pc + 1};
            }
        }
        return RingHandle{};  // all slots taken — reader_id == UINT32_MAX
    }

    // ── unsubscribe ───────────────────────────────────────────────────────────
    // Release a reader slot. The handle is invalidated after this call.
    void unsubscribe(RingHandle const& h) noexcept {
        if (!h.valid()) return;
        producer_.live_readers_bitmap.fetch_and(
            ~(1ULL << h.reader_id), std::memory_order_acq_rel);  // HAL: explicit ordering — see surrounding context
        hal::ctrl_store_release(readers_[h.reader_id].status, ring_detail::kFreeReader);
    }

    // ── send (producer) ───────────────────────────────────────────────────────
    // Non-blocking. Returns RingFull if all slot capacity is in-flight (i.e.,
    // at least one live reader hasn't consumed the oldest slot yet).
    [[nodiscard]] auto send(T const& msg) noexcept
        -> std::expected<void, phyriad::Error>
    {
        if (try_send(msg)) [[likely]] return {};
        return std::unexpected(phyriad::Error{
            .code           = ErrorCode::RingFull,
            .source_node_id = 0,
            .timestamp_ns   = 0});
    }

    // ── try_send — raw bool variant for perf-sensitive hot paths ────────────
    // Same semantics as send() but returns `bool` instead of constructing a
    // 24-byte `std::expected<void, Error>` on every call. Used by the
    // comparison bench against `boost::lockfree::spsc_queue::push` (bool ret).
    //
    // Optimization: closes ~20-30% of the SPSC perf gap by
    // avoiding the expected<void, Error> construct/destruct cost on the
    // success path.
    [[nodiscard, gnu::always_inline]] inline bool
    try_send(T const& msg) noexcept {
        const int64_t next = next_producer_seq_;

        if (next - cached_min_cursor_ > static_cast<int64_t>(Cap)) [[unlikely]] {
            cached_min_cursor_ = min_reader_cursor_();
            if (next - cached_min_cursor_ > static_cast<int64_t>(Cap)) {
                return false;
            }
        }

        hal::prefetch<hal::PrefetchHint::Write>(
            &slots_[static_cast<std::size_t>(
                (next + static_cast<int64_t>(hal::kRingPrefetchSlots)) & mask_)]);

        slots_[static_cast<std::size_t>(next & mask_)] = msg;
        hal::seq_store_release(producer_.cursor, next);
        ++next_producer_seq_;
        return true;
    }

    // ── receive (consumer) ────────────────────────────────────────────────────
    // Non-blocking. Returns RingEmpty if no new messages are available.
    // Advances handle.next_seq and updates the reader cursor for backpressure.
    [[nodiscard]] auto receive(RingHandle& handle) noexcept
        -> std::expected<T, phyriad::Error>
    {
        T out;
        if (try_receive(handle, out)) [[likely]] return out;
        return std::unexpected(phyriad::Error{
            .code           = handle.valid() ? ErrorCode::RingEmpty
                                             : ErrorCode::InvalidHandle,
            .source_node_id = 0,
            .timestamp_ns   = 0});
    }

    // ── try_receive — raw bool variant for perf-sensitive hot paths ─────────
    // Writes the consumed message to `out` and returns true on success;
    // returns false on empty or invalid handle (no message stored in out).
    //
    // Optimization: same rationale as try_send: avoids the
    // expected<T, Error> construct cost on the steady-state success path.
    [[nodiscard, gnu::always_inline]] inline bool
    try_receive(RingHandle& handle, T& out) noexcept {
        if (!handle.valid()) [[unlikely]] return false;

        const int64_t avail = hal::seq_load_acquire(producer_.cursor);
        if (avail < handle.next_seq) [[unlikely]] return false;

        hal::prefetch<hal::PrefetchHint::Read>(
            &slots_[static_cast<std::size_t>(
                (handle.next_seq + static_cast<int64_t>(hal::kRingPrefetchSlots)) & mask_)]);

        out = slots_[static_cast<std::size_t>(handle.next_seq & mask_)];
        hal::seq_store_release(readers_[handle.reader_id].cursor, handle.next_seq);
        ++handle.next_seq;
        return true;
    }

    // ── send_batch (producer, Phase O1.2) ────────────────────────────────────
    // Publish up to msgs.size() messages atomically from the consumer's POV:
    // a single seq_store_release at the end of the batch publishes ALL slots
    // in one fence, amortizing the release barrier across the batch.
    //
    // Returns the number of messages successfully published. On partial
    // success (ring fills mid-batch), returns count < msgs.size(); caller
    // can retry with the tail.
    //
    // Memory ordering rationale:
    //   - Slot writes (plain) for [first..last] are safely unobservable by
    //     any reader because reader's seq_load_acquire(cursor) gates access.
    //   - The single seq_store_release(cursor, last_seq) publishes all slot
    //     writes — release establishes happens-before with reader's acquire.
    //

    [[nodiscard]] uint32_t send_batch(std::span<T const> msgs) noexcept {
        if (msgs.empty()) [[unlikely]] return 0u;

        const int64_t first_seq = next_producer_seq_;
        const int64_t want_last = first_seq + static_cast<int64_t>(msgs.size()) - 1;

        // Backpressure check ONCE for the whole batch.
        // If we exceed capacity, refresh the cached min and retry with a
        // truncated batch that fits in available headroom.
        if (want_last - cached_min_cursor_ > static_cast<int64_t>(Cap)) [[unlikely]] {
            cached_min_cursor_ = min_reader_cursor_();
            const int64_t headroom = static_cast<int64_t>(Cap) -
                                     (first_seq - cached_min_cursor_);
            if (headroom <= 0) return 0u;

            const uint32_t fit = static_cast<uint32_t>(
                std::min<int64_t>(static_cast<int64_t>(msgs.size()), headroom));
            if (fit == 0u) return 0u;
            // Tail-recurse with the truncated span — same invariants apply.
            return send_batch(msgs.subspan(0u, fit));
        }

        // Write all slots without per-message fences.
        for (uint32_t i = 0u; i < msgs.size(); ++i) {
            slots_[static_cast<std::size_t>((first_seq + i) & mask_)] = msgs[i];
        }

        // Single release-store publishes the entire batch.
        hal::seq_store_release(producer_.cursor, want_last);
        next_producer_seq_ = want_last + 1;
        return static_cast<uint32_t>(msgs.size());
    }

    // ── receive_batch (consumer, Phase O1.2) ─────────────────────────────────
    // Drain up to out.size() messages in a single acquire-load + single
    // release-store, amortizing the cursor sync across the batch. Returns the
    // number of messages actually drained into `out`.
    //
    // The acquire load on producer_.cursor establishes visibility of all
    // slot writes up to producer_cursor; reading slots is then safe without
    // per-message fences. The single store_release on the reader's cursor
    // advances backpressure tracking for the producer.
    [[nodiscard]] uint32_t receive_batch(RingHandle& handle,
                                         std::span<T> out) noexcept {
        if (!handle.valid()) [[unlikely]] return 0u;
        if (out.empty())     [[unlikely]] return 0u;

        const int64_t avail = hal::seq_load_acquire(producer_.cursor);
        const int64_t want  = handle.next_seq;
        if (avail < want) [[unlikely]] return 0u;

        const uint32_t count = static_cast<uint32_t>(
            std::min<int64_t>(avail - want + 1,
                              static_cast<int64_t>(out.size())));

        // Prefetch one batch ahead for sustained drains.
        hal::prefetch<hal::PrefetchHint::Read>(
            &slots_[static_cast<std::size_t>(
                (want + static_cast<int64_t>(count)) & mask_)]);

        for (uint32_t i = 0u; i < count; ++i) {
            out[i] = slots_[static_cast<std::size_t>((want + i) & mask_)];
        }

        // Single release advances the reader cursor by `count`.
        hal::seq_store_release(readers_[handle.reader_id].cursor,
                               want + static_cast<int64_t>(count) - 1);
        handle.next_seq = want + static_cast<int64_t>(count);
        return count;
    }

    // ── reader_heartbeat (watchdog hook) ─────────────────────────────────────
    // Reader bumps its heartbeat_seq to signal liveness to the watchdog.
    void reader_heartbeat(RingHandle const& h) noexcept {
        if (!h.valid()) return;
        hal::stat_fetch_add_relaxed(readers_[h.reader_id].heartbeat_seq, uint64_t{1});
    }

    [[nodiscard]] int64_t producer_cursor() const noexcept {
        return hal::seq_load_acquire(producer_.cursor);
    }

private:
    [[nodiscard]] int64_t min_reader_cursor_() const noexcept {
        uint64_t live = hal::seq_load_acquire(producer_.live_readers_bitmap);
        if (live == 0) [[unlikely]] {
            return hal::seq_load_acquire(producer_.cursor);
        }
        int64_t min_c = std::numeric_limits<int64_t>::max();
        while (live) {
            const uint32_t i = static_cast<uint32_t>(std::countr_zero(live));
            live &= live - 1;
            const int64_t c = hal::seq_load_acquire(readers_[i].cursor);
            if (c < min_c) min_c = c;
        }
        return min_c;
    }

    static constexpr int64_t mask_ = static_cast<int64_t>(Cap) - 1;

    // ── Shared state — cache-line isolated ───────────────────────────────────
    alignas(hal::kDestructivePad) ring_detail::ProducerSide producer_;
    alignas(hal::kDestructivePad)
        std::array<ring_detail::ReaderState, kMaxRingReaders> readers_{};
    alignas(hal::kDestructivePad) std::array<T, Cap> slots_{};

    // ── Producer-local state — not shared with any other thread ──────────────
    int64_t next_producer_seq_{0};
    int64_t cached_min_cursor_{-1};
};

} // namespace phyriad::transport
// Made with my soul - Swately <3
