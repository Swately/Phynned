// apps/ayama/core/include/ayama/core/InternalWatchdog.hpp
// InternalWatchdog — background thread that detects main-thread stalls.
//
// The main thread calls heartbeat() each tick. If the watchdog detects
// that no heartbeat was received for > 10 seconds, it logs a critical
// warning and invokes request_recovery() (which currently logs + flags
// the stop condition to break the main loop).
//
// The watchdog thread itself uses std::this_thread::sleep_for(5s) between
// checks — it never spins.
//
// Threading: owns one std::thread (watcher_). heartbeat() is lock-free
//            (atomic store). start/stop must be called from main thread.
// Resource:  ~128B state + 1 thread (dormant 99.9% of the time).
// Privilege: None.
//
#pragma once

#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>

#include <phyriad/hal/Timestamp.hpp>   // phyriad::hal::rdtsc()
#include <phyriad/hal/MemoryOrder.hpp>

namespace ayama::core {

class InternalWatchdog {
public:
    /// Timeout before triggering recovery (seconds).
    static constexpr uint64_t kStallThresholdSeconds = 10u;
    /// How often the watchdog checks (seconds).
    static constexpr uint64_t kPollSeconds            = 5u;

    InternalWatchdog() noexcept = default;

    ~InternalWatchdog() noexcept { stop(); }

    InternalWatchdog(const InternalWatchdog&)            = delete;
    InternalWatchdog& operator=(const InternalWatchdog&) = delete;

    /// Start the watchdog thread.
    /// `tsc_freq` — TSC frequency in Hz (from phyriad::hal::calibrate_tsc_freq()).
    /// `on_stall`  — called on the watchdog thread when stall detected.
    ///               Must be noexcept and non-blocking.
    void start(uint64_t tsc_freq,
               std::function<void()> on_stall = {}) noexcept
    {
        if (phyriad::hal::seq_load_acquire(running_)) return;

        tsc_freq_  = (tsc_freq > 0u) ? tsc_freq : 2'000'000'000ull;
        on_stall_  = std::move(on_stall);
        phyriad::hal::seq_store_release(stop_, false);

        // Seed last_tick so we don't immediately fire.
        phyriad::hal::seq_store_release(last_tick_tsc_, phyriad::hal::rdtsc());

        phyriad::hal::seq_store_release(running_, true);
        watcher_ = std::thread([this]() noexcept { watch_loop(); });
    }

    /// Stop the watchdog thread. Blocks until thread exits.
    void stop() noexcept {
        if (!phyriad::hal::seq_load_acquire(running_)) return;
        phyriad::hal::seq_store_release(stop_, true);
        if (watcher_.joinable()) watcher_.join();
        phyriad::hal::seq_store_release(running_, false);
    }

    /// Call from the main thread each tick to signal liveness.
    void heartbeat() noexcept {
        phyriad::hal::seq_store_release(last_tick_tsc_, phyriad::hal::rdtsc());
    }

    [[nodiscard]] bool running() const noexcept {
        return phyriad::hal::seq_load_acquire(running_);
    }

    [[nodiscard]] uint32_t stall_count() const noexcept {
        return phyriad::hal::stat_load_relaxed(stall_count_);
    }

private:
    std::atomic<uint64_t> last_tick_tsc_{0ull};
    std::atomic<bool>     stop_         {false};
    std::atomic<bool>     running_      {false};
    std::atomic<uint32_t> stall_count_  {0u};

    uint64_t              tsc_freq_     {2'000'000'000ull};
    std::thread           watcher_      {};
    std::function<void()> on_stall_     {};

    void watch_loop() noexcept {
        // Sleep interrumpible: kPollSeconds segundos en chunks de 100 ms
        // para que stop() retorne en ≤ 100 ms en lugar de esperar el ciclo
        // completo. Antes el join() bloqueaba 5 s.
        constexpr std::chrono::milliseconds kSleepChunk{100};
        const uint32_t kChunksPerPoll =
            static_cast<uint32_t>(kPollSeconds * 1000u / 100u);

        while (!phyriad::hal::seq_load_acquire(stop_)) {
            // Sleep en chunks; chequea stop_ cada 100 ms.
            for (uint32_t i = 0u; i < kChunksPerPoll; ++i) {
                if (phyriad::hal::seq_load_acquire(stop_)) return;
                std::this_thread::sleep_for(kSleepChunk);
            }

            // (Re-check tras los chunks por si stop_ se setea justo después
            // del último loop interno.)
            if (phyriad::hal::seq_load_acquire(stop_)) break;

            const uint64_t now  = phyriad::hal::rdtsc();
            const uint64_t last = phyriad::hal::seq_load_acquire(last_tick_tsc_);
            const uint64_t elapsed_ms = (tsc_freq_ > 0u)
                ? (now - last) * 1000ull / tsc_freq_
                : 0ull;

            if (elapsed_ms > kStallThresholdSeconds * 1000ull) {
                phyriad::hal::stat_fetch_add_relaxed(stall_count_, 1u);
                std::fprintf(stderr,
                    "[Ayama][Watchdog] CRITICAL: main thread stalled for %llu ms "
                    "(threshold %llu s). Requesting recovery.\n",
                    static_cast<unsigned long long>(elapsed_ms),
                    static_cast<unsigned long long>(kStallThresholdSeconds));

                if (on_stall_) {
                    on_stall_();
                }
                // Reset timer to avoid spamming logs.
                phyriad::hal::seq_store_release(last_tick_tsc_, phyriad::hal::rdtsc());
            }
        }
    }
};

} // namespace ayama::core
// Made with my soul - Swately <3
