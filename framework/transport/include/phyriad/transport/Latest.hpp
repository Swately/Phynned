// framework/transport/include/phyriad/transport/Latest.hpp
// In-process typed seqlock SWMR — last-value single-slot channel.
//
// Latest<T>: single writer, multiple readers, in-process only.
// Owns its T storage. Address-stable (not movable — contains atomics).
//
// Cache-line layout:
//   seq_write_  alignas(kDestructivePad) — the single sequence; readers check parity
//   data_       alignas(kDestructivePad) — the stored T value
//
// The two alignas specifiers guarantee each field starts on its own destructive-pad
// boundary — the compiler inserts implicit padding between them. No manual pad arrays.
//
// Seqlock protocol (canonical single-writer Rigtorp/Boehm shape; ARM-correct by
// construction, verified on x86 — an aarch64 CI run is the open verification, FR):
//   write(): relaxed odd-bump (in-progress) → seq_fence_release (StoreStore) → copy T →
//            seq_store_release even (publishes T). Single writer owns the sequence, so
//            the odd-bump is a plain store, NOT a `lock xadd`.
//   read():  spin while seq_write_ is odd; copy T; seq_fence_acquire (LoadLoad) →
//            re-read seq_write_ (relaxed); retry if it changed.
//
// Memory ordering — the two DIRECTIONAL barriers a one-way acquire/release cannot give:
//   writer StoreStore (seq_fence_release) pins the odd-bump BEFORE the T write; the
//   closing seq_store_release publishes T (ARM: STLR). reader LoadLoad (seq_fence_acquire)
//   pins the T read BEFORE the seq recheck so it cannot sink past it (ARM: DMB). A bare
//   acquire-load on the recheck left that LoadLoad latent on weak memory models — see
//   CPU_SUBSTRATE_PRIOR_ART.md Appendix A.2.
//
// Satisfies Transport<Latest<T>, T> — static_assert below verifies this.
//

#pragma once
#include "Transport.hpp"
#include <phyriad/schema/Error.hpp>
#include <phyriad/schema/PodMessage.hpp>
#include <phyriad/hal/Cacheline.hpp>
#include <phyriad/hal/MemoryOrder.hpp>
#include <atomic>
#include <cstdint>
#include <expected>

namespace phyriad::transport {

template <schema::PodMessage T>
class Latest {
public:
    static constexpr schema::Hash128 type_hash = schema::schema_hash<T>();

    static constexpr LatencyClass latency_class() noexcept {
        return LatencyClass::LocalCache;
    }

    Latest()  = default;
    ~Latest() = default;

    // Not copyable or movable — contains atomics; object must be address-stable.
    Latest(Latest const&)            = delete;
    Latest& operator=(Latest const&) = delete;
    Latest(Latest&&)                 = delete;
    Latest& operator=(Latest&&)      = delete;

    // ── write (producer) ─────────────────────────────────────────────────────
    // Single writer only. Non-blocking. Call only from the producer thread.
    void write(T const& v) noexcept {
        // Single-writer seqlock (canonical Rigtorp/Boehm shape). The writer is the sole
        // mutator of the sequence, so the odd-bump is a plain relaxed store — NOT a
        // serializing `lock xadd`. seq_fence_release pins the odd-bump before the payload
        // (StoreStore); the closing seq_store_release publishes the payload to readers.
        const uint64_t s = phyriad::hal::stat_load_relaxed(seq_write_);
        phyriad::hal::seq_store_relaxed(seq_write_, s + 1u);  // odd → write in progress
        phyriad::hal::seq_fence_release();                    // StoreStore: odd-bump BEFORE data_
        data_ = v;                                            // non-atomic; single writer owns data_
        phyriad::hal::seq_store_release(seq_write_, s + 2u);  // even → published (release fences data_)
    }

    // ── read (any reader, concurrent) ────────────────────────────────────────
    // Returns a consistent snapshot. Spins only during active concurrent writes.
    // Lock-free: the spin is bounded by the producer's write duration.
    [[nodiscard]] T read() const noexcept {
        T result{};
        while (true) {
            const uint64_t s1 = phyriad::hal::seq_load_acquire(seq_write_);
            if (s1 & 1u) [[unlikely]] {  // odd = write in progress
                hal::spin_hint();
                continue;
            }
            result = data_;
            phyriad::hal::seq_fence_acquire();  // LoadLoad: pin the payload read BEFORE the recheck
            const uint64_t s2 = phyriad::hal::stat_load_relaxed(seq_write_);
            if (s1 == s2) [[likely]] break;  // no concurrent write during copy
        }
        return result;
    }

    // ── Transport concept interface ───────────────────────────────────────────
    [[nodiscard]] auto send(T const& v) noexcept
        -> std::expected<void, phyriad::Error>
    {
        write(v);
        return {};
    }

    [[nodiscard]] auto receive() noexcept
        -> std::expected<T, phyriad::Error>
    {
        return read();
    }

private:
    // Each alignas(kDestructivePad) member starts on its own cache-line boundary.
    // The compiler inserts implicit padding between fields to enforce this.
    // (The former `seq_read_` mirror was write-only — written every publish, read
    // nowhere — a vestige of a superseded two-counter design; removed, −1 pad region.)
    alignas(hal::kDestructivePad) std::atomic<uint64_t> seq_write_{0};
    alignas(hal::kDestructivePad) T                     data_{};
};

// Verify Transport concept is satisfied for the canonical sample type.
static_assert(Transport<Latest<schema::SampleTick>, schema::SampleTick>,
    "Latest<SampleTick> must satisfy Transport");

} // namespace phyriad::transport
// Made with my soul - Swately <3
