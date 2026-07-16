// framework/stigmergy/include/phyriad/stigmergy/Worker.hpp
// phyriad::stigmergy::Worker<Logic> — autonomous-agent worker loop helper.
//
// Helper template for the most common stigmergic-worker pattern:
//   while (running) {
//     auto observation = read_field_or_pheromone();
//     auto work_result = do_work(observation);
//     deposit_or_publish(work_result);
//   }
//
// `Logic` is a user-provided callable returning void:
//
//   void Logic::operator()(std::stop_token) noexcept;
//
// The worker calls Logic exactly once per iteration; Logic is responsible
// for the actual read-decide-deposit cycle. The Worker class just owns
// the std::jthread + lifetime + optional pin-to-core.
//
// Why a thin wrapper and not a full base class:
//   * Forces composition (the agent's logic is plain data + lambdas, not
//     a hierarchy)
//   * Keeps the primitive scope to "lifetime + thread", not "what agents do"
//   * If a user wants different cadence (sleep-based, event-driven, etc.)
//     they write their own thread loop instead of using Worker
//
// This is an OPTIONAL convenience — not load-bearing for the pattern.
// Users can implement workers however they want; Field<T> and
// Pheromone<T,N> work with any thread implementation.
//
// Pinning policy (Problem 6 of PILLAR_COMPOSABILITY_AUDIT):
//   To keep stigmergy standalone (no topology dep), pinning is performed
//   through a caller-supplied function pointer. A consumer that wants pin
//   support passes `&phyriad::hw::pin_current_thread` (from the topology
//   pillar) at construction; consumers that do not care leave `pin_fn` null
//   and skip pinning entirely. Either way, stigmergy itself never depends on
//   topology.
//
// Stigmergy as first-class pillar.
#pragma once
#include <cstdint>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace phyriad::stigmergy {

// ── PinFn — caller-supplied pin function ─────────────────────────────────────
// Signature matches `phyriad::hw::pin_current_thread(uint32_t) noexcept`.
// `Worker::start()` invokes `pin_fn(pin_to_core)` from inside the spawned
// thread when both are set. nullptr → no pinning.
using PinFn = void (*)(uint32_t) noexcept;

/// Autonomous-agent thread wrapper.
///
/// Logic must be invocable as `void logic(std::stop_token st)`. The
/// jthread invokes it once at startup; Logic itself runs the loop and
/// observes `st.stop_requested()` to exit cleanly on shutdown.
///
/// Optional `pin_to_core` + `pin_fn` cause the thread to set its affinity to a
/// specific logical core at startup — useful when the agent reads/writes
/// a specific Pheromone slot and you want it co-located with the rest of
/// the agents on the same CCD/L3 domain. Both must be set for pinning to
/// happen; either being absent (default) leaves the OS scheduler in charge.
template <typename Logic>
    requires std::is_invocable_r_v<void, Logic, std::stop_token>
class Worker {
public:
    /// Default-construct an idle Worker. Call `start()` to launch.
    Worker() noexcept = default;

    /// Construct + launch in one step (most common case).
    explicit Worker(Logic   logic,
                    uint32_t pin_to_core = UINT32_MAX,
                    PinFn    pin_fn      = nullptr) noexcept
        : logic_{std::move(logic)}
        , pin_to_core_{pin_to_core}
        , pin_fn_{pin_fn}
    {
        start();
    }

    ~Worker() noexcept { stop_and_join(); }

    Worker(Worker const&)            = delete;
    Worker& operator=(Worker const&) = delete;
    Worker(Worker&&)                 = default;
    Worker& operator=(Worker&&)      = default;

    /// Launch the underlying jthread. Idempotent.
    void start() noexcept {
        if (thread_.joinable()) return;
        thread_ = std::jthread{[this](std::stop_token st) noexcept {
            if (pin_fn_ != nullptr && pin_to_core_ != UINT32_MAX) {
                pin_fn_(pin_to_core_);
            }
            logic_(st);
        }};
    }

    /// Request stop + join. Idempotent.
    void stop_and_join() noexcept {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
    }

    [[nodiscard]] bool running() const noexcept {
        return thread_.joinable();
    }

private:
    Logic        logic_{};
    uint32_t     pin_to_core_{UINT32_MAX};
    PinFn        pin_fn_{nullptr};
    std::jthread thread_{};
};

// Deduction guide so `Worker w{lambda};` works without specifying Logic.
template <typename Logic>
Worker(Logic, uint32_t = UINT32_MAX, PinFn = nullptr) -> Worker<Logic>;

} // namespace phyriad::stigmergy
// Made with my soul - Swately <3
