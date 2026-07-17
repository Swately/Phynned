// core/tests/agent_idle_budget_test.cpp
// Automated CI test covering idle budget cases (5s run instead of 24h —
// sufficient to detect leaks/runaway CPU).
//
// T4 "Phynned no-op idle":     agent CPU% during idle ≤ 5% (CI margin).
// T6 "Degradation-safe":     start() OK without admin → no crash → clean stop().
// T9 "Resource budget":      ΔRSS during 5s idle < 10 MB.
//
// Does NOT validate real targets / ETW (requires admin + real processes) —
// only the base agent loop and its resource footprint.
#include <phynned/core/AgentRuntime.hpp>
#include <phyriad/tuning/WorkingSet.hpp>     // FR-19
#include <phyriad/process/CurrentProcess.hpp> // FR-18

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
namespace {
inline uint64_t now_user_kernel_100ns() noexcept {
    FILETIME ct, et, kt, ut;
    if (!GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) return 0u;
    const uint64_t k = (static_cast<uint64_t>(kt.dwHighDateTime) << 32) | kt.dwLowDateTime;
    const uint64_t u = (static_cast<uint64_t>(ut.dwHighDateTime) << 32) | ut.dwLowDateTime;
    return k + u;
}
} // anon ns
#else
#  include <time.h>
#  include <sys/resource.h>
namespace {
inline uint64_t now_user_kernel_100ns() noexcept {
    struct rusage r{};
    if (getrusage(RUSAGE_SELF, &r) != 0) return 0u;
    const uint64_t us =
        static_cast<uint64_t>(r.ru_utime.tv_sec) * 1'000'000ull +
        static_cast<uint64_t>(r.ru_utime.tv_usec) +
        static_cast<uint64_t>(r.ru_stime.tv_sec) * 1'000'000ull +
        static_cast<uint64_t>(r.ru_stime.tv_usec);
    return us * 10ull;  // 1 µs = 10 × 100 ns
}
} // anon ns
#endif

static int g_failures = 0;
#define CHECK(cond, msg)                                                   \
    do { if (!(cond)) {                                                    \
        std::fprintf(stderr, "[FAIL] %s\n", msg);                          \
        ++g_failures;                                                      \
    } } while (0)

int main() {
    using namespace std::chrono_literals;
    using phynned::core::AgentConfig;
    using phynned::core::AgentRuntime;

    std::printf("agent_idle_budget_test starting (pid=%u, %s)\n",
                phyriad::proc::self_pid(), phyriad::proc::self_name());

    // ── Baseline measurements ────────────────────────────────────────────
    uint64_t rss_before = 0u, rss_peak_before = 0u;
    (void)phyriad::tuning::get_self_working_set(&rss_before, &rss_peak_before);
    const uint64_t cpu_before_100ns = now_user_kernel_100ns();
    const auto    wall_before        = std::chrono::steady_clock::now();

    // ── T6 / T9: start agent sin admin, en modo CI-friendly ──────────────
    AgentConfig cfg{};
    cfg.require_admin           = false;   // T6 path: degraded
    cfg.enable_shm_publish      = false;   // sin SHM en unit test
    cfg.self_pin_to_slow_cores  = false;   // no tocar affinity del runner

    AgentRuntime agent(cfg);
    {
        auto r = agent.start();
        CHECK(r.has_value(),
              "T6: AgentRuntime::start() debe retornar Ok aún sin admin");
        if (!r.has_value()) {
            std::fprintf(stderr,
                "  start() error code=%u — saltando resto del test.\n",
                static_cast<unsigned>(r.error().code));
            return g_failures;
        }
        std::printf("[OK] T6: start() Ok sin admin\n");
    }

    // ── Idle loop por 5 segundos en thread aparte ────────────────────────
    std::thread runner([&agent]() { agent.run(); });
    std::this_thread::sleep_for(5s);

    // ── Snapshot mid-run para chequear que ticks corrieron ───────────────
    const uint32_t ticks_during = agent.tick_count();
    CHECK(ticks_during > 0u,
          "T4: tick_count() debe ser > 0 después de 5s (loop activo)");
    std::printf("[OK] T4: tick_count() = %u después de 5s idle\n", ticks_during);

    // ── stop() y verificar tiempo de exit ────────────────────────────────
    const auto stop_request = std::chrono::steady_clock::now();
    agent.stop();
    runner.join();
    const auto stop_done = std::chrono::steady_clock::now();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        stop_done - stop_request).count();

    // El loop tickea cada 1000 ms en Idle; aceptar hasta 2000 ms para CI lento.
    CHECK(stop_ms < 2000,
          "T1/T6: stop() debe completar en < 2 s desde la solicitud");
    std::printf("[OK] T1/T6: stop() completó en %lld ms\n",
                static_cast<long long>(stop_ms));

    CHECK(!agent.running(),
          "T1: running() debe ser false después de stop()/join");
    std::printf("[OK] T1: running() == false post-stop\n");

    // ── T9: resource budget — ΔRSS y CPU% durante el run ─────────────────
    uint64_t rss_after = 0u, rss_peak_after = 0u;
    (void)phyriad::tuning::get_self_working_set(&rss_after, &rss_peak_after);
    const uint64_t cpu_after_100ns = now_user_kernel_100ns();
    const auto    wall_after        = std::chrono::steady_clock::now();

    const int64_t drss_kb =
        (static_cast<int64_t>(rss_after) - static_cast<int64_t>(rss_before))
        / 1024;
    const uint64_t wall_delta_100ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            wall_after - wall_before).count() / 100ull;
    const uint64_t cpu_delta_100ns = cpu_after_100ns - cpu_before_100ns;
    const double cpu_pct = (wall_delta_100ns > 0u)
        ? static_cast<double>(cpu_delta_100ns) /
          static_cast<double>(wall_delta_100ns) * 100.0
        : 0.0;

    std::printf("[INFO] RSS: %llu KB → %llu KB (Δ %+lld KB; peak %llu KB)\n",
                static_cast<unsigned long long>(rss_before / 1024u),
                static_cast<unsigned long long>(rss_after / 1024u),
                static_cast<long long>(drss_kb),
                static_cast<unsigned long long>(rss_peak_after / 1024u));
    std::printf("[INFO] CPU: %.2f%% sobre 5s\n", cpu_pct);

    // T9 budget (versión CI-relajada):
    //   ΔRSS < 10 MB es muy generoso — el binary debería estar steady.
    //   CPU% < 5% absoluto — spec dice 0.5% pero CI compartidos pueden subir.
    CHECK(drss_kb < 10240,
          "T9: ΔRSS durante 5s idle debe ser < 10 MB (10240 KB)");
    if (drss_kb < 10240) std::printf("[OK] T9: ΔRSS = %+lld KB < 10240 KB\n",
                                     static_cast<long long>(drss_kb));

    CHECK(cpu_pct < 5.0,
          "T4: CPU% durante 5s idle debe ser < 5% (hard limit relajado para CI)");
    if (cpu_pct < 5.0) std::printf("[OK] T4: CPU%% = %.2f%% < 5%%\n", cpu_pct);

    std::printf("\nagent_idle_budget_test completó con %d fallos\n", g_failures);
    if (g_failures == 0) std::printf("[PASS] agent_idle_budget_test\n");
    return g_failures;
}
// Made with my soul - Swately <3
