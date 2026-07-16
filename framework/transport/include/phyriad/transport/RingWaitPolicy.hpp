// framework/transport/include/phyriad/transport/RingWaitPolicy.hpp
// Wait policies for blocking ring operations (publish_block / consume_block).
//
// Policy structs with a single static `wait(uint64_t spins)` method.
// Zero overhead — the compiler inlines and eliminates dead branches when
// used as a template parameter.
//
// Available policies (least → most cooperative):
//   RingWaitBusySpin    — pure spin, no pause instruction
//   RingWaitPause       — PAUSE/YIELD (x86/ARM hint), reduces bus pressure
//   RingWaitYield       — std::this_thread::yield() — OS scheduler quantum
//   RingWaitBackoff     — adaptive: spin → pause → yield → sleep(1µs)
//   RingWaitSleeping    — immediate 10µs sleep (idle/power-save mode)
//
// Uses hal::spin_hint() for platform-correct PAUSE/YIELD — never raw _mm_pause.
//
// Usage:
//   ring.consume_block<RingWaitBackoff>(dst, handle);
//   ring.publish_block<RingWaitBusySpin>(msg);
//

#pragma once
#include <phyriad/hal/MemoryOrder.hpp>   // hal::spin_hint()
#include <chrono>
#include <cstdint>
#include <thread>

namespace phyriad::transport {

// ── RingWaitBusySpin ─────────────────────────────────────────────────────────
// Pure spin — no yield, no pause. Maximum responsiveness when a dedicated
// hardware thread is assigned to this ring.
struct RingWaitBusySpin {
    static inline void wait(uint64_t /*spins*/) noexcept {
        // Intentionally empty: spin at full speed.
    }
};

// ── RingWaitPause ─────────────────────────────────────────────────────────────
// x86 PAUSE / ARM YIELD hint. Reduces bus coherence traffic during spin-wait
// without yielding the core. Latency ~20-40 ns better than BusySpin under HT.
// Use for producers waiting on nearly-full rings, or very short waits.
struct RingWaitPause {
    static inline void wait(uint64_t /*spins*/) noexcept {
        hal::spin_hint();  // expands to PAUSE (x86) or YIELD (ARM)
    }
};

// ── RingWaitYield ─────────────────────────────────────────────────────────────
// Yields the current scheduler time-slice. Latency ~1-10 µs depending on OS.
// Use for non-latency-critical consumers that must not burn a full core.
struct RingWaitYield {
    static inline void wait(uint64_t /*spins*/) noexcept {
        std::this_thread::yield();
    }
};

// ── RingWaitBackoff ───────────────────────────────────────────────────────────
// Adaptive: spin → pause → yield → sleep(1µs).
// Transitions are driven by the number of consecutive failed attempts.
// Recommended default for general-purpose consumers — good latency/CPU balance.
struct RingWaitBackoff {
    // Configurable thresholds (iterations without progress before escalation).
    static constexpr uint64_t kSpinLimit  = 200;   // pure spin
    static constexpr uint64_t kPauseLimit = 400;   // PAUSE hint
    static constexpr uint64_t kYieldLimit = 420;   // yield

    static inline void wait(uint64_t spins) noexcept {
        if (spins < kSpinLimit) {
            return;  // Phase 1: pure spin
        }
        if (spins < kPauseLimit) {
            hal::spin_hint();  // Phase 2: PAUSE/YIELD hint
            return;
        }
        if (spins < kYieldLimit) {
            std::this_thread::yield();  // Phase 3: OS quantum yield
            return;
        }
        // Phase 4: sleep 1µs — for sustained long waits (ring empty / slow producer).
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
};

// ── RingWaitSleeping ──────────────────────────────────────────────────────────
// Immediate 10µs sleep on every failed attempt.
// Use for idle workers or POWER_SAVE mode where latency is irrelevant.
struct RingWaitSleeping {
    static inline void wait(uint64_t /*spins*/) noexcept {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
};

} // namespace phyriad::transport
// Made with my soul - Swately <3
