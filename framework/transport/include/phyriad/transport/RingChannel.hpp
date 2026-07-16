// framework/transport/include/phyriad/transport/RingChannel.hpp
// RingChannel<T,Capacity,MaxReaders> — in-process MPMC ring with multi-reader
// support. Disruptor-style multi-producer: a CAS-claimed shared producer cursor +
// a gating sequence (the min reader cursor) for capacity + a per-slot publication
// sequence, with reader cursors per subscriber. (NOT Vyukov bounded-MPMC, whose
// per-slot sequence gates ADMISSION head-of-line; here the shared cursor is claimed
// by CAS and a separate gating sequence bounds capacity — the LMAX MP model.)
//
// ── Compared to Ring<T,Cap> (SWMR Disruptor) ──────────────────────────────
//   Ring<T,Cap>       — Single Writer Multi Reader (SWMR). Producer is a single
//                       thread; readers can be up to 64. Used by Pool.
//   RingChannel<...>  — Multi Producer Multi Reader (MPMC). Multiple producers
//                       contend for slots via atomic fetch_add on producer_cursor.
//                       Use when you have multiple producer threads writing into
//                       the same channel (e.g. fan-in from N submitter threads
//                       into a single shared queue).
//
// ── Algorithm (Disruptor multi-producer: CAS cursor + gating seq + per-slot publish) ─
//   Producer (CAS-based claim — never rolls back the shared cursor):
//     1. pos = producer_cursor.load; claimed = pos + 1
//     2. Capacity gate: claimed - min_reader_cursor > Capacity → return false
//        (NO claim is made — the cursor is untouched, so no hole is created)
//     3. CAS producer_cursor pos→claimed; on failure reload pos and retry
//     4. Write slots[claimed & mask].data = msg
//     5. slots[claimed & mask].seq.store(claimed, release)
//
//   Why CAS instead of fetch_add+rollback: the old path did fetch_add to
//   claim, then fetch_sub to roll back on a cap-fail. Under 2-producer
//   contention this left a PERMANENT HOLE in the sequence space — producer A
//   could roll back claim N while producer B already held claim N+1, so the
//   value N was never written. The consumer reads strictly in cursor order
//   and waited for seq==N forever → intermittent livelock (always at
//   Capacity=1024, ~1-in-4 at 4096). CAS reserves each `claimed` value
//   exclusively and contiguously, so every seq is eventually published and
//   the in-order consumer never stalls.
//
//   Consumer (per reader):
//     1. next = reader_cursors[id].load(acquire) + 1
//     2. seq = slots[next & mask].seq.load(acquire)
//     3. If seq != next → slot not ready, return empty
//     4. Read slots[next & mask].data
//     5. reader_cursors[id].store(next, release)
//
// ── Subscribe / Unsubscribe ───────────────────────────────────────────────
//   subscribe() walks live_readers_bitmap looking for a FREE slot (bit==0).
//   On claim, the reader cursor is initialised to producer_cursor (skip the
//   backlog — new readers see only new messages). unsubscribe() clears the
//   bit so the producer's min_cursor scan ignores this slot's stale cursor.
//
// ── Cache-line layout (anti-false-sharing) ────────────────────────────────
//   producer_cursor / live_readers_bitmap / reader_cursors[i] each occupy
//   their own cache line via alignas(hal::kDestructivePad). Critical for
//   N-reader fan-out perf — MESI invalidations stay local to each line.
//
//  to in-process std::array storage. SHM-specific concerns (RealmLayout,
//  external buffer ownership, kSlotsOffset) removed; algorithm preserved.
#pragma once
#include "RingWaitPolicy.hpp"
#include <phyriad/hal/Cacheline.hpp>
#include <phyriad/hal/MemoryOrder.hpp>
#include <atomic>
#include <array>
#include <cstdint>
#include <optional>

namespace phyriad::transport {

// ── RingChannel<T, Capacity, MaxReaders> ─────────────────────────────────
template <typename T,
          std::size_t Capacity,
          std::size_t MaxReaders = 16>
class RingChannel {
    static_assert((Capacity & (Capacity - 1)) == 0,
        "RingChannel: Capacity must be a power of 2");
    static_assert(Capacity >= 2, "RingChannel: Capacity must be >= 2");
    static_assert(MaxReaders >= 1 && MaxReaders <= 64,
        "RingChannel: MaxReaders must be in [1, 64]");

public:
    static constexpr std::size_t kCapacity   = Capacity;
    static constexpr std::size_t kMaxReaders = MaxReaders;
    static constexpr int64_t     kMask       = static_cast<int64_t>(Capacity) - 1;

    // Reader handle returned by subscribe(). Opaque to callers.
    struct ReaderHandle {
        uint32_t id{UINT32_MAX};
        [[nodiscard]] bool valid() const noexcept { return id < MaxReaders; }
    };

    // Default ctor: all atomics value-initialise to their declared defaults
    // (Slot::seq{-1}, CursorSlot::value{-1}, producer_cursor_{-1},
    // live_readers_{0}). No explicit loops needed.
    RingChannel() noexcept = default;

    RingChannel(RingChannel const&)            = delete;
    RingChannel& operator=(RingChannel const&) = delete;
    RingChannel(RingChannel&&)                 = delete;
    RingChannel& operator=(RingChannel&&)      = delete;

    // ── Subscribe / Unsubscribe ──────────────────────────────────────────
    // Claim a reader slot. Returns a valid handle on success, or invalid
    // (id == UINT32_MAX) if all MaxReaders slots are in use.
    [[nodiscard]] ReaderHandle subscribe() noexcept {
        for (uint32_t i = 0; i < MaxReaders; ++i) {
            const uint64_t bit = 1ULL << i;
            // HAL: acq_rel fetch_or — synchronises slot claim with producer's
            // min_cursor scan via live_readers_bitmap.
            uint64_t prev = live_readers_.fetch_or(bit, std::memory_order_acq_rel);  // HAL: acq_rel CAS — atomic state transition
            if (!(prev & bit)) {
                // Initialise this reader's cursor to producer_cursor — skip
                // the backlog (new subscribers see only new messages).
                const int64_t pc =
                    hal::seq_load_acquire(producer_cursor_);
                hal::seq_store_release(reader_cursors_[i].value, pc);
                return ReaderHandle{i};
            }
        }
        return ReaderHandle{};   // invalid
    }

    void unsubscribe(ReaderHandle h) noexcept {
        if (!h.valid()) return;
        const uint64_t bit = 1ULL << h.id;
        live_readers_.fetch_and(~bit, std::memory_order_acq_rel);  // HAL: acq_rel CAS — atomic state transition
    }

    // ── publish — single message, non-blocking ────────────────────────────
    // Returns false if the ring is full (a live reader is Capacity behind).
    // Thread-safe across multiple producer threads.
    //
    // CAS-based "cap-check-then-claim" — replaces the old fetch_add+rollback,
    // which created permanent holes in the sequence space under contention and
    // livelocked the in-order consumer (see the file-header note). A `claimed`
    // value is reserved ONLY by a successful CAS, so claims are exclusive and
    // contiguous and no hole is ever produced; a cap-fail returns false without
    // touching the cursor.
    //
    // Cap-gate (unchanged semantics): `min_reader_cursor_()` returns INT64_MIN
    // when no readers are live (broadcaster free-fill OK); otherwise the
    // minimum live reader cursor (-1 if any is still at the sentinel). Slot
    // index `claimed & mask` last held seq `claimed - Capacity`; the gate
    // `claimed - min_rdr <= Capacity` guarantees the slowest reader has already
    // stored its cursor past that value (hence finished reading the old data),
    // so overwriting the slot is safe. Reader cursors only advance, so the
    // gate is monotone: once it passes for a given `claimed` it stays passed.
    [[nodiscard]] bool publish(T const& msg) noexcept {
        int64_t pos     = hal::stat_load_relaxed(producer_cursor_);
        int64_t min_rdr = min_reader_cursor_();
        int64_t claimed = pos + 1;
        for (;;) {
            claimed = pos + 1;
            if (min_rdr != INT64_MIN &&
                claimed - min_rdr > static_cast<int64_t>(Capacity)) {
                // Re-scan once: readers may have advanced since the snapshot.
                min_rdr = min_reader_cursor_();
                if (min_rdr != INT64_MIN &&
                    claimed - min_rdr > static_cast<int64_t>(Capacity))
                    return false;   // genuinely full — no claim made, no hole
            }
            if (producer_cursor_.compare_exchange_weak(
                    pos, claimed,
                    std::memory_order_acq_rel,    // HAL: acq_rel CAS — exclusive slot claim, replaces fetch_add+rollback
                    std::memory_order_relaxed))   // HAL: relaxed on CAS fail — pos is reloaded, ordering carried by the next acq_rel attempt
                break;
            // CAS failed: `pos` now holds the current cursor — retry the claim.
        }

        // Write data, then publish via seq.store(release) — the consumer
        // sees the data only after observing seq == claimed.
        Slot& slot = slots_[static_cast<std::size_t>(claimed & kMask)];
        slot.data = msg;
        hal::seq_store_release(slot.seq, claimed);
        return true;
    }

    // ── publish_block<Wait> — blocking publish with wait policy ───────────
    template <typename Wait = RingWaitPause>
    void publish_block(T const& msg) noexcept {
        uint32_t spins = 0;
        while (!publish(msg)) {
            Wait::wait(spins++);
        }
    }

    // ── consume — single message, non-blocking ────────────────────────────
    // Returns the next message for the given reader, or nullopt if no new
    // message is available. Each reader sees ALL messages published after
    // its subscribe() call.
    [[nodiscard]] std::optional<T> consume(ReaderHandle h) noexcept {
        T out;
        if (try_consume(h, out)) return out;
        return std::nullopt;
    }

    // ── try_consume — raw bool variant for perf-sensitive hot paths ───────
    // Writes the consumed message to `out` and returns true on success;
    // returns false when no new message is available or handle is invalid
    // (no message stored in out).
    //
    // STL-avoidance audit fix.
    // Avoids the `std::optional<T>` construct cost (size_of(T) + 1 byte tag
    // + alignment padding) on the steady-state success path. For uint64_t
    // payloads this saves the 1-byte-tag store + alignment-aware branch.
    // Used by perf-sensitive consumers (bench_ring_channel, comparison bench,
    // pool's ResultDispatcher) to match the bare-pointer return semantics
    // of `boost::lockfree::queue::pop(out)`.
    //
    // The existing `consume()` (returning std::optional) is preserved
    // unchanged and now delegates here, so existing callers see no change.
    [[nodiscard, gnu::always_inline]] inline bool
    try_consume(ReaderHandle h, T& out) noexcept {
        if (!h.valid()) [[unlikely]] return false;
        const int64_t cur  = hal::seq_load_acquire(reader_cursors_[h.id].value);
        const int64_t next = cur + 1;

        Slot const& slot = slots_[static_cast<std::size_t>(next & kMask)];
        const int64_t seq = hal::seq_load_acquire(slot.seq);
        if (seq != next) return false;   // slot not yet published

        out = slot.data;
        hal::seq_store_release(reader_cursors_[h.id].value, next);
        return true;
    }

    // ── consume_block<Wait> — blocking consume with wait policy ───────────
    template <typename Wait = RingWaitPause>
    T consume_block(ReaderHandle h) noexcept {
        uint32_t spins = 0;
        for (;;) {
            if (auto opt = consume(h); opt) return *opt;
            Wait::wait(spins++);
        }
    }

    // ── Queries ──────────────────────────────────────────────────────────
    [[nodiscard]] int64_t producer_cursor() const noexcept {
        return hal::seq_load_acquire(producer_cursor_);
    }

    [[nodiscard]] uint64_t live_readers_mask() const noexcept {
        return hal::seq_load_acquire(live_readers_);
    }

    // Approximate fill — newest published minus slowest reader.
    // Handle the new INT64_MIN ("no readers") sentinel
    // explicitly. -1 (subscribed but at sentinel cursor) is treated as
    // "0 consumed", so `pending() == pc + 1` matches the producer's view.
    [[nodiscard]] int64_t pending() const noexcept {
        const int64_t pc = producer_cursor();
        const int64_t mr = min_reader_cursor_();
        if (mr == INT64_MIN) return pc + 1;   // no readers ⇒ producer's full backlog
        return pc - mr;                       // works for mr=-1 (=> pc + 1) too
    }

private:
    struct alignas(hal::kDestructivePad) Slot {
        std::atomic<int64_t> seq{-1};
        T                    data{};
    };

    // Scan live_readers_bitmap and return the minimum reader_cursor.
    //
    // Sentinel discipline:
    //   INT64_MIN  → no readers live (broadcaster pattern; producer free-fills).
    //   -1         → at least one reader live AND its cursor is still at the
    //                 sentinel (subscribed but never consumed). Capacity MUST
    //                 be enforced for the producer.
    //   ≥ 0        → minimum consumed cursor across all live readers.
    //
    // The previous design returned -1 for both "no readers" and "readers at
    // sentinel" — `publish()` couldn't distinguish them, and the capacity
    // gate was bypassed in the latter case, causing producer overrun.
    [[nodiscard]] int64_t min_reader_cursor_() const noexcept {
        const uint64_t live = hal::seq_load_acquire(live_readers_);
        if (live == 0) return INT64_MIN;

        int64_t min_c = INT64_MAX;
        for (uint32_t i = 0; i < MaxReaders; ++i) {
            if (!(live & (1ULL << i))) continue;
            // CursorSlot's conversion operators don't participate in template
            // argument deduction — read .value directly.
            const int64_t c = hal::seq_load_acquire(reader_cursors_[i].value);
            if (c < min_c) min_c = c;
        }
        // `min_c == INT64_MAX` would only happen if `live` was non-zero but no
        // matching bit had a valid cursor — impossible given subscribe()/
        // unsubscribe() invariants. Defensive return INT64_MIN.
        return (min_c == INT64_MAX) ? INT64_MIN : min_c;
    }

    // Cache-line aligned cursors (anti-false-sharing).
    alignas(hal::kDestructivePad) std::atomic<int64_t> producer_cursor_{-1};
    alignas(hal::kDestructivePad) std::atomic<uint64_t> live_readers_{0};

    // Each reader cursor on its own cache line — anti-false-sharing across
    // the N-reader fan-out. Wrapper provides implicit conversion to
    // std::atomic<int64_t>& so hal::seq_load_acquire/store_release work
    // transparently with reader_cursors_[i].
    struct alignas(hal::kDestructivePad) CursorSlot {
        std::atomic<int64_t> value{-1};
        char _pad[hal::kDestructivePad - sizeof(std::atomic<int64_t>)];

        operator std::atomic<int64_t>&() noexcept { return value; }
        operator std::atomic<int64_t> const&() const noexcept { return value; }
    };
    std::array<CursorSlot, MaxReaders> reader_cursors_{};

    // Slot storage (in-process — no SHM offset like legacy).
    std::array<Slot, Capacity> slots_{};
};

} // namespace phyriad::transport
// Made with my soul - Swately <3
