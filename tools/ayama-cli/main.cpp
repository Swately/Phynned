// apps/ayama/tools/ayama-cli/main.cpp
// ayama-cli — command-line interface for ayama-agent control and analysis.
//
// Commands:
//   status                          Show agent status (connected / disconnected)
//   targets                         List currently observed targets
//   actions                         Show recent action log
//   bench diff <a> <b>              Compare two ayama-bench CSV outputs
//   presentmon-import <pm.csv>      Parse PresentMon CSV → ayama-bench format
//                  [--process <name|pid>] [--output <file>]
//   memory list                     Show per-game learned entries (reads file directly)
//   memory clear                    Clear all per-game memory
//   memory clear-bad                Remove all bad-list entries
//   memory clear-audit              Delete the binary audit log (audit.bin)
//   help                            Show this help
//

#include <ayama/ipc/AyamaClient.hpp>
#include <ayama/learn/PerGameMemory.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // Sleep() for IPC ack-wait loop
#else
#include <time.h>     // nanosleep
#endif

// ── Forward declarations ─────────────────────────────────────────────────
static int cmd_status();
static int cmd_targets();
static int cmd_actions();
static int cmd_bench_diff(const char* file_a, const char* file_b);
static int cmd_memory_list();
static int cmd_memory_clear();
static int cmd_memory_clear_bad();
static int cmd_memory_clear_audit();
static int cmd_pause();
static int cmd_resume();
static int cmd_revert_all();
static void print_help();

// ── CSV diff helpers ─────────────────────────────────────────────────────
struct BenchMetrics {
    float    avg_ms             {0.0f};
    float    p50_ms             {0.0f};
    float    p99_ms             {0.0f};
    float    p999_ms            {0.0f};
    float    max_ms             {0.0f};
    float    variance_ms        {0.0f};
    uint32_t stutter_count      {0u};   ///< frames > 2× avg (relative)
    uint32_t stutter_abs_60hz   {0u};   ///< frames > 16.67 ms (absolute, sub-60Hz hitches)
    uint32_t total_frames       {0u};
    bool     valid              {false};
};

static BenchMetrics load_bench_csv(const char* path) noexcept
{
    BenchMetrics m;
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "Error: cannot open '%s'\n", path);
        return m;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char key[64]{};
        double val = 0.0;
        if (std::sscanf(line, "%63[^,],%lf", key, &val) == 2) {
            if      (std::strcmp(key, "avg_ms")           == 0) m.avg_ms      = static_cast<float>(val);
            else if (std::strcmp(key, "p50_ms")           == 0) m.p50_ms      = static_cast<float>(val);
            else if (std::strcmp(key, "p99_ms")           == 0) m.p99_ms      = static_cast<float>(val);
            else if (std::strcmp(key, "p999_ms")          == 0) m.p999_ms     = static_cast<float>(val);
            else if (std::strcmp(key, "max_ms")           == 0) m.max_ms      = static_cast<float>(val);
            else if (std::strcmp(key, "variance_ms")      == 0) m.variance_ms = static_cast<float>(val);
            else if (std::strcmp(key, "stutter_count")    == 0) m.stutter_count = static_cast<uint32_t>(val);
            else if (std::strcmp(key, "stutter_abs_60hz") == 0) m.stutter_abs_60hz = static_cast<uint32_t>(val);
            else if (std::strcmp(key, "total_frames")     == 0) m.total_frames  = static_cast<uint32_t>(val);
        }
    }
    std::fclose(f);
    m.valid = true;
    return m;
}

static float pct_change(float baseline, float treated) noexcept
{
    if (baseline <= 0.001f) return 0.0f;
    return (baseline - treated) / baseline * 100.0f;
}

// ── Command implementations ───────────────────────────────────────────────
static int cmd_status()
{
    ayama::ipc::AyamaClient client;
    const auto r = client.connect();
    if (!r.has_value()) {
        std::fprintf(stdout,
            "Status: DISCONNECTED\n"
            "  Agent not running or SHM unavailable.\n"
            "  Launch ayama-agent.exe to enable optimization.\n");
        return 1;
    }

    const ayama::ipc::AyamaStateHeader* state = client.state();
    if (!state) {
        std::fprintf(stderr, "Error: SHM connected but state unavailable.\n");
        return 1;
    }

    static const char* kPrivLevel[] = { "None", "Partial", "Admin", "Admin" };
    static const char* kBenchPhase[] = { "Idle", "Recording A", "Recording B", "Done" };

    std::fprintf(stdout,
        "Status: CONNECTED\n"
        "  Privilege:       %s\n"
        "  ETW active:      %s\n"
        "  Targets:         %u\n"
        "  Active decisions:%u\n"
        "  Bench phase:     %s\n"
        "  Pressure:        %.2f\n",
        kPrivLevel[state->privilege_level > 3u ? 3u : state->privilege_level],
        state->etw_active ? "yes" : "no",
        state->n_targets,
        state->n_decisions,
        kBenchPhase[state->bench_phase > 3u ? 0u : state->bench_phase],
        static_cast<double>(state->aggregate_pressure));
    return 0;
}

static int cmd_targets()
{
    ayama::ipc::AyamaClient client;
    if (!client.connect().has_value()) {
        std::fprintf(stderr, "Error: Agent not running.\n");
        return 1;
    }

    const auto targets = client.targets();
    if (targets.empty()) {
        std::fprintf(stdout, "No observed targets.\n");
        return 0;
    }

    std::fprintf(stdout,
        "%-8s  %-32s  %-12s  %-10s\n",
        "PID", "Name", "Kind", "Status");
    // ASCII separator (PowerShell cp1252/850 no renderiza Unicode box chars).
    std::fprintf(stdout,
        "--------  --------------------------------  ------------  ----------\n");

    static const char* kKindNames[] = {
        "Unknown", "Game", "Stream", "Comm",
        "Browser", "Productivity", "System"
    };
    static const char* kStatusNames[] = { "Running", "Suspended", "Exiting" };

    for (const auto& t : targets) {
        const uint8_t ki = static_cast<uint8_t>(t.kind);
        const uint8_t si = static_cast<uint8_t>(t.status);
        std::fprintf(stdout,
            "%-8u  %-32s  %-12s  %s\n",
            t.pid, t.name,
            (ki < 7u) ? kKindNames[ki]   : "?",
            (si < 3u) ? kStatusNames[si] : "?");
    }
    return 0;
}

static int cmd_actions()
{
    ayama::ipc::AyamaClient client;
    if (!client.connect().has_value()) {
        std::fprintf(stderr, "Error: Agent not running.\n");
        return 1;
    }

    const auto log = client.action_log();
    // Filter out empty entries.
    bool any = false;
    for (const auto& a : log) {
        if (a.target_pid == 0u && a.tsc_applied == 0u) continue;
        if (!any) {
            std::fprintf(stdout,
                "%-8s  %-8s  %-18s  %-18s  %s\n",
                "PID", "Rule", "AffinityMask", "PrevMask", "Status");
            std::fprintf(stdout,
                "--------  --------  ------------------  ------------------  --------\n");
            any = true;
        }
        std::fprintf(stdout,
            "%-8u  %-8u  0x%-16llx  0x%-16llx  %s\n",
            a.target_pid, a.rule_id,
            static_cast<unsigned long long>(a.new_affinity_mask),
            static_cast<unsigned long long>(a.prev_affinity_mask),
            a.tsc_reverted != 0u ? "reverted" :
            a.success              ? "active"   : "failed");
    }
    if (!any) std::fprintf(stdout, "No actions recorded.\n");
    return 0;
}

// ── IPC commands ─────────────────────────────────────────────────────────
// Send a command to the running agent via the SHM command slot and wait
// for ack. Used by the UI bench runner AND for manual testing/debugging.
static int send_ipc_command_(uint32_t cmd_kind, const char* label) {
    ayama::ipc::AyamaClient client;
    if (!client.connect().has_value()) {
        std::fprintf(stderr, "Error: Agent not running.\n");
        return 1;
    }
    if (!client.can_send_commands()) {
        std::fprintf(stderr,
            "Error: SHM mapped read-only — cannot send commands.\n"
            "(Likely the agent was started with a stricter ACL, or this build\n"
            " is mismatched. Restart agent with current binary.)\n");
        return 1;
    }
    const uint64_t seq = client.send_command(cmd_kind);
    if (seq == 0ull) {
        std::fprintf(stderr, "Error: send_command returned 0 (no client).\n");
        return 1;
    }
    // Wait up to 2 seconds for ack.
    std::fprintf(stdout, "[ayama-cli] %s sent (seq=%llu). Waiting ack...\n",
                 label, static_cast<unsigned long long>(seq));
    for (int i = 0; i < 200; ++i) {  // 200 × 10ms = 2 s
        if (client.command_ack() >= seq) {
            std::fprintf(stdout, "[ayama-cli] Agent ack'd.\n");
            return 0;
        }
#ifdef _WIN32
        Sleep(10);
#else
        struct timespec ts{0, 10'000'000};
        nanosleep(&ts, nullptr);
#endif
    }
    std::fprintf(stderr,
        "[ayama-cli] WARN: no ack within 2 s. Command may still be applied;\n"
        "  check `ayama-cli status` and `ayama-cli actions`.\n");
    return 2;
}

static int cmd_pause() {
    return send_ipc_command_(ayama::ipc::kAyamaCmdPausePolicies,
                             "pause policies");
}
static int cmd_resume() {
    return send_ipc_command_(ayama::ipc::kAyamaCmdResumePolicies,
                             "resume policies");
}
static int cmd_revert_all() {
    return send_ipc_command_(ayama::ipc::kAyamaCmdForceRevertAll,
                             "force revert all");
}

// ── Multi-run statistical aggregation ────────────────────────────────────
// For rigorous A/B/A/B/A testing across multiple runs, computes:
//   - Mean + stddev across runs
//   - 95% confidence interval (mean ± 1.96 * stderr)
//   - Cross-group delta + significance flag (CI no-overlap)
struct GroupStat {
    double mean;
    double stddev;
    double stderr_;   // stddev / sqrt(n)
    double ci95_low;
    double ci95_high;
    uint32_t n;
};

static GroupStat compute_group_stat(const std::vector<double>& vs) noexcept
{
    GroupStat s{};
    s.n = static_cast<uint32_t>(vs.size());
    if (s.n == 0u) return s;

    double sum = 0.0;
    for (double v : vs) sum += v;
    s.mean = sum / static_cast<double>(s.n);

    if (s.n < 2u) {
        s.stddev   = 0.0;
        s.stderr_  = 0.0;
        s.ci95_low = s.mean;
        s.ci95_high = s.mean;
        return s;
    }

    double ss = 0.0;
    for (double v : vs) {
        const double d = v - s.mean;
        ss += d * d;
    }
    // Sample stddev (n-1).
    s.stddev = std::sqrt(ss / static_cast<double>(s.n - 1u));
    s.stderr_ = s.stddev / std::sqrt(static_cast<double>(s.n));
    // 95% CI assuming normal approximation (z=1.96). Reasonable for n≥3.
    // For small n we'd use t-distribution; t(0.025, 4 df) = 2.776, but
    // we keep 1.96 as a conservative single-formula approach. Note for
    // user: if n is very small (2 or 3), CI is wide.
    s.ci95_low  = s.mean - 1.96 * s.stderr_;
    s.ci95_high = s.mean + 1.96 * s.stderr_;
    return s;
}

static const char* significance_label(double a_low, double a_high,
                                       double b_low, double b_high,
                                       bool lower_is_better) noexcept
{
    // If 95% CIs don't overlap → statistically significant.
    if (lower_is_better) {
        if (b_high < a_low) return "SIGNIFICANT (treated < baseline)";
        if (b_low > a_high) return "SIGNIFICANT (treated > baseline)";
    } else {
        if (b_low > a_high) return "SIGNIFICANT (treated > baseline)";
        if (b_high < a_low) return "SIGNIFICANT (treated < baseline)";
    }
    return "WITHIN CONFIDENCE INTERVAL (no statistically detectable difference)";
}

static int cmd_bench_multi(const std::vector<const char*>& baselines,
                            const std::vector<const char*>& treateds) noexcept
{
    if (baselines.size() < 2u || treateds.size() < 2u) {
        std::fprintf(stderr,
            "Error: need at least 2 baseline + 2 treated CSVs for statistical aggregate.\n"
            "       Got %zu baseline, %zu treated.\n",
            baselines.size(), treateds.size());
        return 1;
    }

    // Load all CSVs.
    auto load_group = [](const std::vector<const char*>& files,
                         const char* label)
        -> std::vector<BenchMetrics>
    {
        std::vector<BenchMetrics> v;
        v.reserve(files.size());
        std::fprintf(stdout, "Loading %s group (%zu files):\n", label, files.size());
        for (const char* f : files) {
            BenchMetrics m = load_bench_csv(f);
            if (!m.valid) {
                std::fprintf(stderr, "  [FAIL] %s\n", f);
                continue;
            }
            std::fprintf(stdout, "  [OK] %s — avg=%.2f P99=%.2f P99.9=%.2f hitches=%u\n",
                f, static_cast<double>(m.avg_ms),
                static_cast<double>(m.p99_ms),
                static_cast<double>(m.p999_ms),
                m.stutter_abs_60hz);
            v.push_back(m);
        }
        return v;
    };

    const auto base = load_group(baselines, "baseline");
    const auto treat = load_group(treateds, "treated");
    if (base.size() < 2u || treat.size() < 2u) {
        std::fprintf(stderr, "Error: not enough valid CSVs loaded.\n");
        return 1;
    }

    // Extract per-metric vectors (inline lambdas for each metric — simple and clear).
    auto extract_avg  = [](const std::vector<BenchMetrics>& gs) {
        std::vector<double> v; v.reserve(gs.size());
        for (const auto& g : gs) v.push_back(static_cast<double>(g.avg_ms));
        return v;
    };
    auto extract_p99  = [](const std::vector<BenchMetrics>& gs) {
        std::vector<double> v; v.reserve(gs.size());
        for (const auto& g : gs) v.push_back(static_cast<double>(g.p99_ms));
        return v;
    };
    auto extract_p999 = [](const std::vector<BenchMetrics>& gs) {
        std::vector<double> v; v.reserve(gs.size());
        for (const auto& g : gs) v.push_back(static_cast<double>(g.p999_ms));
        return v;
    };
    auto extract_hitch = [](const std::vector<BenchMetrics>& gs) {
        std::vector<double> v; v.reserve(gs.size());
        for (const auto& g : gs) v.push_back(static_cast<double>(g.stutter_abs_60hz));
        return v;
    };
    // max_ms extractor: captures cross-CCD migration outliers and PSO-compile
    // spikes that P99.9 cannot — with 14k frames per run, P99.9 only sees
    // the worst 14 frames, diluting rare-but-severe outliers. max_ms catches
    // them directly.
    auto extract_max = [](const std::vector<BenchMetrics>& gs) {
        std::vector<double> v; v.reserve(gs.size());
        for (const auto& g : gs) v.push_back(static_cast<double>(g.max_ms));
        return v;
    };

    const GroupStat avg_b   = compute_group_stat(extract_avg(base));
    const GroupStat avg_t   = compute_group_stat(extract_avg(treat));
    const GroupStat p99_b   = compute_group_stat(extract_p99(base));
    const GroupStat p99_t   = compute_group_stat(extract_p99(treat));
    const GroupStat p999_b  = compute_group_stat(extract_p999(base));
    const GroupStat p999_t  = compute_group_stat(extract_p999(treat));
    const GroupStat hitch_b = compute_group_stat(extract_hitch(base));
    const GroupStat hitch_t = compute_group_stat(extract_hitch(treat));
    const GroupStat max_b   = compute_group_stat(extract_max(base));
    const GroupStat max_t   = compute_group_stat(extract_max(treat));

    auto delta_pct = [](double from, double to) -> double {
        return (from > 0.001) ? (from - to) / from * 100.0 : 0.0;
    };

    std::fprintf(stdout,
        "\n-- ayama-bench multi-run aggregate -----------------------------\n"
        "  Baseline: n=%u runs   Treated: n=%u runs\n\n"
        "  Frame time (lower = better)\n"
        "  ----------------------------------------------------------------\n"
        "  Metric    Baseline mean    Treated mean    Delta    Significance\n",
        avg_b.n, avg_t.n);

    auto print_row = [&](const char* name, const GroupStat& b, const GroupStat& t,
                          bool lower_is_better) {
        const double d = delta_pct(b.mean, t.mean);
        const char* sig = significance_label(b.ci95_low, b.ci95_high,
                                              t.ci95_low, t.ci95_high,
                                              lower_is_better);
        std::fprintf(stdout, "  %-7s  %9.2f ± %.2f  %9.2f ± %.2f  %+6.1f%%  %s\n",
            name,
            b.mean, b.ci95_high - b.mean,   // ± half-width of 95%CI
            t.mean, t.ci95_high - t.mean,
            d, sig);
    };

    print_row("Avg",   avg_b,  avg_t,  true);
    print_row("P99",   p99_b,  p99_t,  true);
    print_row("P99.9", p999_b, p999_t, true);
    // max_ms reveals spike elimination (cross-CCD migration outliers, PSO
    // compiles) that the percentile metrics don't.
    // Caveat: max_ms can have high baseline variance from PSO compile
    // spikes (e.g. Halo 2 Run 1 = 58 ms cold spike). The SIG check
    // reflects this honestly.
    print_row("Max",   max_b,  max_t,  true);

    std::fprintf(stdout,
        "\n  Stutters (lower = better)\n"
        "  ----------------------------------------------------------------\n");
    print_row("hitch>16.67", hitch_b, hitch_t, true);

    std::fprintf(stdout,
        "\n  Per-run dispersion (high stddev = noisy test; consider more runs):\n"
        "  ----------------------------------------------------------------\n");
    std::fprintf(stdout,
        "  Avg   stddev: baseline=%.2f ms  treated=%.2f ms\n"
        "  P99   stddev: baseline=%.2f ms  treated=%.2f ms\n"
        "  P99.9 stddev: baseline=%.2f ms  treated=%.2f ms\n"
        "  Max   stddev: baseline=%.2f ms  treated=%.2f ms\n"
        "  Hitch stddev: baseline=%.1f    treated=%.1f\n",
        avg_b.stddev,  avg_t.stddev,
        p99_b.stddev,  p99_t.stddev,
        p999_b.stddev, p999_t.stddev,
        max_b.stddev,  max_t.stddev,
        hitch_b.stddev, hitch_t.stddev);

    // Overall verdict — based on P99.9 (deep tail = player-perceived stutters)
    // AND max_ms (spike elimination — only signal that captures rare outliers).
    //
    // Expanded verdict logic: the previous logic missed the Borderlands 2
    // high-FPS case where:
    //   - P99 was SIGNIFICANT
    //   - max_ms went 14 ms (baseline) → 6 ms (treated) — 50% reduction
    //   - P99.9 was within CI (rare spikes diluted in 14k-frame "worst 14")
    // The old verdict said NULL despite obvious improvement. Now max_ms is
    // a verdict signal in its own right.
    const bool p999_significant = (p999_t.ci95_high < p999_b.ci95_low);
    const bool p99_significant  = (p99_t.ci95_high < p99_b.ci95_low);
    const bool max_significant  = (max_t.ci95_high < max_b.ci95_low);
    const bool any_regression_significant =
        (p999_t.ci95_low > p999_b.ci95_high) ||
        (hitch_t.ci95_low > hitch_b.ci95_high) ||
        (max_t.ci95_low  > max_b.ci95_high);

    std::fprintf(stdout, "\n  Verdict (multi-run, statistical):\n  ");
    if (any_regression_significant) {
        std::fprintf(stdout, "REGRESSION DETECTED (P99.9, max, or hitches CI exceeds baseline upper bound)\n");
    } else if (p999_significant) {
        std::fprintf(stdout, "SIGNIFICANT IMPROVEMENT (P99.9 CI strictly below baseline CI)\n");
    } else if (p99_significant && max_significant) {
        // BL2 high-FPS pattern: spike elimination + P99 improvement, but
        // P99.9 doesn't capture the spikes because they're rare relative
        // to total frame count.
        std::fprintf(stdout,
            "SIGNIFICANT IMPROVEMENT — spike elimination "
            "(P99 + max-frame-time both CI no-overlap; P99.9 unchanged but "
            "max went %.1f → %.1f ms)\n",
            max_b.mean, max_t.mean);
    } else if (p99_significant && delta_pct(p999_b.mean, p999_t.mean) > 0) {
        std::fprintf(stdout, "MARGINAL IMPROVEMENT (P99 significant, P99.9 trending positive but CI overlap)\n");
    } else if (max_significant && delta_pct(p99_b.mean, p99_t.mean) > 5.0) {
        // Edge case: max SIG, P99 trending strongly positive but CI overlap.
        std::fprintf(stdout,
            "MARGINAL IMPROVEMENT — spike reduction "
            "(max CI no-overlap, P99 trending +%.1f%% within CI)\n",
            delta_pct(p99_b.mean, p99_t.mean));
    } else {
        std::fprintf(stdout, "NO STATISTICALLY DETECTABLE DIFFERENCE (CIs overlap)\n");
    }
    std::fprintf(stdout,
        "----------------------------------------------------------------\n\n");
    return 0;
}

static int cmd_bench_diff(const char* file_a, const char* file_b)
{
    const BenchMetrics a = load_bench_csv(file_a);
    const BenchMetrics b = load_bench_csv(file_b);
    if (!a.valid || !b.valid) return 1;

    // Frametime deltas (positive = improvement = lower FT in treated).
    const float avg_delta  = pct_change(a.avg_ms,      b.avg_ms);
    const float p99_delta  = pct_change(a.p99_ms,      b.p99_ms);
    const float p999_delta = pct_change(a.p999_ms,     b.p999_ms);
    const float p50_delta  = pct_change(a.p50_ms,      b.p50_ms);
    const float max_delta  = pct_change(a.max_ms,      b.max_ms);
    const float var_delta  = pct_change(a.variance_ms, b.variance_ms);

    // FPS conversions (1000 / FT_ms).
    auto to_fps = [](float ft_ms) -> double {
        return (ft_ms > 0.001f) ? 1000.0 / static_cast<double>(ft_ms) : 0.0;
    };
    const double avg_fps_a   = to_fps(a.avg_ms);
    const double avg_fps_b   = to_fps(b.avg_ms);
    const double low_1_a     = to_fps(a.p99_ms);
    const double low_1_b     = to_fps(b.p99_ms);
    const double low_01_a    = to_fps(a.p999_ms);
    const double low_01_b    = to_fps(b.p999_ms);
    const float avg_fps_delta = pct_change(static_cast<float>(avg_fps_b),
                                            static_cast<float>(avg_fps_a)); // FPS: higher=better
    const float low_1_delta   = pct_change(static_cast<float>(low_1_b),
                                            static_cast<float>(low_1_a));
    const float low_01_delta  = pct_change(static_cast<float>(low_01_b),
                                            static_cast<float>(low_01_a));

    // Stutter deltas. Note: "stutter_count" uses 2×avg threshold (relative)
    // which becomes noisy when avg itself shifts; "stutter_abs_60hz" counts
    // frames > 16.67 ms (sub-60Hz hitches) — platform-independent.
    const float stt_rel_delta = pct_change(static_cast<float>(a.stutter_count),
                                            static_cast<float>(b.stutter_count));
    const float stt_abs_delta = pct_change(static_cast<float>(a.stutter_abs_60hz),
                                            static_cast<float>(b.stutter_abs_60hz));

    // Verdict — based on P99.9 (the deep tail = what players feel as "lag spikes").
    // P99 confirms direction; absolute stutter count is the platform-independent
    // sanity check. Treat the strongest deterioration of any metric as veto.
    const char* verdict;
    if (p999_delta >= 10.0f && p99_delta >= 2.0f && stt_abs_delta >= -10.0f) {
        verdict = "SIGNIFICANT IMPROVEMENT";
    } else if (p999_delta >= 3.0f && p99_delta >= 0.0f) {
        verdict = "MARGINAL IMPROVEMENT";
    } else if (p999_delta <= -3.0f || stt_abs_delta <= -25.0f) {
        verdict = "REGRESSION";
    } else {
        verdict = "NEUTRAL";
    }

    std::fprintf(stdout,
        "\n-- ayama-bench diff ------------------------------------------\n"
        "  Baseline : %s (%u frames)\n"
        "  Treated  : %s (%u frames)\n\n",
        file_a, a.total_frames,
        file_b, b.total_frames);

    // ── Player-perceived FPS panel ────────────────────────────────────────
    std::fprintf(stdout,
        "  Player-perceived FPS (higher = better)\n"
        "  %-20s  %10s  %10s  %10s\n"
        "  --------------------  ----------  ----------  ----------\n",
        "Metric", "Baseline", "Treated", "Delta");
    std::fprintf(stdout, "  %-20s  %10.1f  %10.1f  %+9.1f%%\n",
        "Avg FPS",       avg_fps_a, avg_fps_b, static_cast<double>(avg_fps_delta));
    std::fprintf(stdout, "  %-20s  %10.1f  %10.1f  %+9.1f%%\n",
        "1% low FPS",    low_1_a,   low_1_b,   static_cast<double>(low_1_delta));
    std::fprintf(stdout, "  %-20s  %10.1f  %10.1f  %+9.1f%%\n",
        "0.1% low FPS",  low_01_a,  low_01_b,  static_cast<double>(low_01_delta));

    // ── Frame-time panel (lower = better) ────────────────────────────────
    std::fprintf(stdout,
        "\n  Frame time (ms) -- lower = better\n"
        "  %-20s  %10s  %10s  %10s\n"
        "  --------------------  ----------  ----------  ----------\n",
        "Metric", "Baseline", "Treated", "Delta");
    std::fprintf(stdout, "  %-20s  %10.2f  %10.2f  %+9.1f%%\n",
        "Median (P50)",  static_cast<double>(a.p50_ms),  static_cast<double>(b.p50_ms),  static_cast<double>(p50_delta));
    std::fprintf(stdout, "  %-20s  %10.2f  %10.2f  %+9.1f%%\n",
        "Avg",           static_cast<double>(a.avg_ms),  static_cast<double>(b.avg_ms),  static_cast<double>(avg_delta));
    std::fprintf(stdout, "  %-20s  %10.2f  %10.2f  %+9.1f%%\n",
        "P99",           static_cast<double>(a.p99_ms),  static_cast<double>(b.p99_ms),  static_cast<double>(p99_delta));
    std::fprintf(stdout, "  %-20s  %10.2f  %10.2f  %+9.1f%%\n",
        "P99.9",         static_cast<double>(a.p999_ms), static_cast<double>(b.p999_ms), static_cast<double>(p999_delta));
    std::fprintf(stdout, "  %-20s  %10.2f  %10.2f  %+9.1f%%\n",
        "Max",           static_cast<double>(a.max_ms),  static_cast<double>(b.max_ms),  static_cast<double>(max_delta));
    std::fprintf(stdout, "  %-20s  %10.2f  %10.2f  %+9.1f%%\n",
        "Std Dev",       static_cast<double>(a.variance_ms), static_cast<double>(b.variance_ms), static_cast<double>(var_delta));

    // ── Stutter panel ─────────────────────────────────────────────────────
    std::fprintf(stdout,
        "\n  Stutters (lower = better)\n"
        "  %-20s  %10s  %10s  %10s\n"
        "  --------------------  ----------  ----------  ----------\n",
        "Metric", "Baseline", "Treated", "Delta");
    if (a.stutter_abs_60hz > 0u || b.stutter_abs_60hz > 0u) {
        std::fprintf(stdout, "  %-20s  %10u  %10u  %+9.1f%%\n",
            "Frames > 16.67 ms",  a.stutter_abs_60hz, b.stutter_abs_60hz, static_cast<double>(stt_abs_delta));
    } else {
        std::fprintf(stdout, "  %-20s  %10s  %10s  %10s\n",
            "Frames > 16.67 ms",  "n/a (old)", "n/a (old)", "(re-import)");
    }
    std::fprintf(stdout, "  %-20s  %10u  %10u  %+9.1f%%\n",
        "> 2x avg (relative)", a.stutter_count, b.stutter_count, static_cast<double>(stt_rel_delta));
    if (b.stutter_count > a.stutter_count && b.avg_ms < a.avg_ms) {
        std::fprintf(stdout,
            "        Note: \"> 2x avg\" rose because avg improved (threshold tightened).\n"
            "              The absolute-threshold row above is the platform-independent indicator.\n");
    }

    std::fprintf(stdout,
        "\n  Verdict: %s\n"
        "        (P99.9 frametime delta = %+.1f%% drives verdict; verified vs absolute stutter)\n"
        "-------------------------------------------------------------\n\n",
        verdict, static_cast<double>(p999_delta));

    return (std::strcmp(verdict, "REGRESSION") == 0) ? 2 : 0;
}

static int cmd_memory_list()
{
    char memory_path[512]{};
#ifdef _WIN32
    const char* lad = ::getenv("LOCALAPPDATA");
    if (lad) std::snprintf(memory_path, sizeof(memory_path), "%s\\Ayama\\memory.toml", lad);
#else
    const char* home = ::getenv("HOME");
    if (home) std::snprintf(memory_path, sizeof(memory_path), "%s/.config/ayama/memory.toml", home);
#endif

    if (memory_path[0] == '\0') {
        std::fprintf(stderr, "Error: cannot determine config directory.\n");
        return 1;
    }

    std::FILE* f = std::fopen(memory_path, "r");
    if (!f) {
        std::fprintf(stdout, "No memory.toml found at: %s\n", memory_path);
        return 0;
    }
    std::fprintf(stdout, "-- memory.toml: %s --\n\n", memory_path);
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::fputs(line, stdout);
    }
    std::fclose(f);
    return 0;
}

static int cmd_memory_clear()
{
    std::fprintf(stdout,
        "WARNING: This will delete all per-game learned optimisations.\n"
        "Type 'yes' to confirm: ");
    std::fflush(stdout);

    char confirm[16]{};
    if (std::fgets(confirm, sizeof(confirm), stdin) &&
        std::strncmp(confirm, "yes", 3) == 0)
    {
        char path[512]{};
#ifdef _WIN32
        const char* lad = ::getenv("LOCALAPPDATA");
        if (lad) std::snprintf(path, sizeof(path), "%s\\Ayama\\memory.toml", lad);
#else
        const char* home = ::getenv("HOME");
        if (home) std::snprintf(path, sizeof(path), "%s/.config/ayama/memory.toml", home);
#endif
        if (std::remove(path) == 0) {
            std::fprintf(stdout, "Cleared: %s\n", path);
        } else {
            std::fprintf(stderr, "Error removing file (may not exist): %s\n", path);
        }
    } else {
        std::fprintf(stdout, "Aborted.\n");
    }
    return 0;
}

static int cmd_memory_clear_bad()
{
    ayama::learn::PerGameMemory mem;
    mem.generate_hardware_id();
    const auto load_r = mem.load();
    if (!load_r.has_value()) {
        std::fprintf(stderr, "Could not load memory.toml (may not exist yet).\n");
        return 1;
    }
    const uint32_t n = mem.bad_count();
    mem.clear_all_bad();
    const auto save_r = mem.save();
    if (!save_r.has_value()) {
        std::fprintf(stderr, "Could not save memory.toml.\n");
        return 1;
    }
    std::fprintf(stdout,
        "Cleared %u bad-list entries. Ayama will retry all previously "
        "regressed exes.\n", n);
    return 0;
}

static int cmd_memory_clear_audit()
{
    char path[512]{};
#ifdef _WIN32
    const char* lad = ::getenv("LOCALAPPDATA");
    if (lad) std::snprintf(path, sizeof(path), "%s\\Ayama\\audit.bin", lad);
#else
    const char* home = ::getenv("HOME");
    if (home) std::snprintf(path, sizeof(path), "%s/.config/ayama/audit.bin", home);
#endif
    if (path[0] == '\0') {
        std::fprintf(stderr, "Error: cannot determine config directory.\n");
        return 1;
    }

    // Report current size before removal.
    std::FILE* f = std::fopen(path, "rb");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fclose(f);
        const long n_records = (sz > 0) ? sz / 96 : 0;
        std::fprintf(stdout,
            "Audit log: %s\n"
            "  Size:    %ld bytes  (%ld records × 96B)\n",
            path, sz, n_records);
    } else {
        std::fprintf(stdout, "No audit.bin found at: %s\n", path);
        return 0;
    }

    std::fprintf(stdout, "Delete audit.bin? Type 'yes' to confirm: ");
    std::fflush(stdout);

    char confirm[16]{};
    if (std::fgets(confirm, sizeof(confirm), stdin) &&
        std::strncmp(confirm, "yes", 3) == 0)
    {
        if (std::remove(path) == 0) {
            std::fprintf(stdout, "Cleared: %s\n", path);
        } else {
            std::fprintf(stderr, "Error removing: %s\n", path);
            return 1;
        }
    } else {
        std::fprintf(stdout, "Aborted.\n");
    }
    return 0;
}

// ── presentmon-import — convertir CSV de PresentMon a formato ayama-bench ──
//
// PresentMon (Microsoft, github.com/GameTechDev/PresentMon) captura frame
// times de cualquier app DXGI/Vulkan/D3D9/D3D11/D3D12 vía ETW. Su output CSV
// tiene una row por Present() event con columnas como `Application`,
// `ProcessID`, `msBetweenPresents`, etc. El nombre exacto de columnas varía
// entre versiones de PresentMon, pero `msBetweenPresents` (o
// `MsBetweenPresents` en 2.0+) es el frame time.
//
// Esta función:
//   1. Lee la header row, encuentra los índices de las columnas que necesita.
//   2. Filtra rows por `Application` o `ProcessID` (si el usuario especifica).
//   3. Acumula los `msBetweenPresents` válidos (> 0).
//   4. Computa avg / p99 / p999 / stddev / stutter_count.
//   5. Escribe output en formato ayama-bench (key,value lines).
//
struct PresentMonStats {
    double  avg_ms;
    double  p50_ms;
    double  p99_ms;
    double  p999_ms;
    double  max_ms;
    double  stddev_ms;
    uint32_t total_frames;
    uint32_t stutter_count;      // frames > 2 × avg (relative, platform-shift sensitive)
    uint32_t stutter_abs_60hz;   // frames > 16.67 ms (absolute, sub-60Hz hitches)
};

static int find_column(const char* header_line, const char* col_name) noexcept
{
    // Devuelve el índice (0-based) de la columna `col_name` en `header_line`,
    // o -1 si no se encuentra. Comparación case-insensitive.
    char buf[2048]{};
    std::strncpy(buf, header_line, sizeof(buf) - 1u);
    int idx = 0;
    char* tok = std::strtok(buf, ",\r\n");
    while (tok) {
        // case-insensitive compare
        const char* a = tok;
        const char* b = col_name;
        while (*a && *b) {
            const char la = (*a >= 'A' && *a <= 'Z') ? (*a | 0x20) : *a;
            const char lb = (*b >= 'A' && *b <= 'Z') ? (*b | 0x20) : *b;
            if (la != lb) break;
            ++a; ++b;
        }
        if (*a == '\0' && *b == '\0') return idx;
        tok = std::strtok(nullptr, ",\r\n");
        ++idx;
    }
    return -1;
}

static const char* get_field(const char* line, int col_idx) noexcept
{
    // Devuelve un puntero al inicio del campo `col_idx` en `line`. NO null-
    // terminates — el caller debe parsear hasta la siguiente coma.
    int idx = 0;
    while (*line && idx < col_idx) {
        if (*line == ',') ++idx;
        ++line;
    }
    return (idx == col_idx) ? line : nullptr;
}

static int cmd_presentmon_import(const char* pm_csv,
                                  const char* filter_process,
                                  uint32_t   filter_pid,
                                  const char* output) noexcept
{
    std::FILE* f = std::fopen(pm_csv, "r");
    if (!f) {
        std::fprintf(stderr, "Error: cannot open '%s'\n", pm_csv);
        return 1;
    }

    char line[4096]{};
    // ── 1. Header row ─────────────────────────────────────────────────────
    if (!std::fgets(line, sizeof(line), f)) {
        std::fprintf(stderr, "Error: empty file or no header.\n");
        std::fclose(f);
        return 1;
    }

    // PresentMon column name variants:
    //   v1 (--v1_metrics):   msBetweenPresents
    //   v2 (default 2.x):    FrameTime
    //   ambos:               Application, ProcessID
    //   v1 only:             Dropped (en v2 los dropped se filtran via DisplayedTime)
    int idx_app  = find_column(line, "Application");
    int idx_pid  = find_column(line, "ProcessID");

    // Frame time column — try v1 first, then v2 names.
    int idx_ftms = find_column(line, "msBetweenPresents");
    if (idx_ftms < 0) idx_ftms = find_column(line, "MsBetweenPresents");
    if (idx_ftms < 0) idx_ftms = find_column(line, "FrameTime");   // v2_metrics
    if (idx_ftms < 0) idx_ftms = find_column(line, "msBetweenDisplayChange");  // fallback

    int idx_dropped = find_column(line, "Dropped");                // solo v1
    // v2: si hay DisplayedTime y está vacío → frame dropped. Detectamos abajo.
    int idx_displayed = find_column(line, "DisplayedTime");

    if (idx_ftms < 0) {
        std::fprintf(stderr,
            "Error: ninguna columna de frame time encontrada (probé:\n"
            "  msBetweenPresents, MsBetweenPresents, FrameTime, msBetweenDisplayChange).\n"
            "¿Es output de PresentMon? Header recibido:\n  %s",
            line);
        std::fclose(f);
        return 1;
    }
    std::fprintf(stdout,
        "[ayama-cli] Header parsed. PresentMon format detected:\n"
        "  app col=%d   pid col=%d   frame-time col=%d  dropped col=%d   displayed col=%d\n",
        idx_app, idx_pid, idx_ftms, idx_dropped, idx_displayed);

    // ── 2. Data rows ──────────────────────────────────────────────────────
    std::vector<float> frame_times;
    frame_times.reserve(60'000);   // ~10 min at 100 FPS

    uint32_t rows_read = 0u, rows_kept = 0u, rows_dropped = 0u;
    while (std::fgets(line, sizeof(line), f)) {
        ++rows_read;

        // ── Filter por proceso (si el usuario lo especificó) ────────────
        if (filter_process != nullptr && idx_app >= 0) {
            const char* app = get_field(line, idx_app);
            if (!app) continue;
            // case-insensitive prefix match (PresentMon a veces incluye .exe, a veces no)
            const char* a = app;
            const char* b = filter_process;
            bool match = true;
            while (*b && *a && *a != ',') {
                const char la = (*a >= 'A' && *a <= 'Z') ? (*a | 0x20) : *a;
                const char lb = (*b >= 'A' && *b <= 'Z') ? (*b | 0x20) : *b;
                if (la != lb) { match = false; break; }
                ++a; ++b;
            }
            if (!match || *b != '\0') continue;
        }
        if (filter_pid > 0u && idx_pid >= 0) {
            const char* pid_field = get_field(line, idx_pid);
            if (!pid_field) continue;
            const uint32_t pid = static_cast<uint32_t>(std::atoi(pid_field));
            if (pid != filter_pid) continue;
        }

        // ── Skip dropped frames ─────────────────────────────────────────
        // v1: columna explícita "Dropped" = 0/1.
        // v2: si DisplayedTime está vacío (campo "" entre comas) → dropped.
        if (idx_dropped >= 0) {
            const char* dropped = get_field(line, idx_dropped);
            if (dropped && (*dropped == '1' || *dropped == 't' || *dropped == 'T')) {
                ++rows_dropped;
                continue;
            }
        } else if (idx_displayed >= 0) {
            const char* disp = get_field(line, idx_displayed);
            if (disp && (*disp == ',' || *disp == '\r' || *disp == '\n' || *disp == '\0')) {
                ++rows_dropped;
                continue;
            }
        }

        // ── Parse msBetweenPresents ─────────────────────────────────────
        const char* ft = get_field(line, idx_ftms);
        if (!ft) continue;
        const float frame_ms = static_cast<float>(std::atof(ft));
        if (frame_ms <= 0.0f) continue;      // skip primera row (no delta)
        if (frame_ms > 5000.0f) continue;    // > 5 s = pause/breakpoint, exclude

        frame_times.push_back(frame_ms);
        ++rows_kept;
    }
    std::fclose(f);

    if (frame_times.empty()) {
        std::fprintf(stderr,
            "Error: 0 frames después del filter. Verifica:\n"
            "  - PresentMon capturó el proceso correcto (col Application=%s)\n"
            "  - El filter --process '%s' o --pid %u coincide\n"
            "  Rows leídas: %u  (dropped: %u)\n",
            idx_app >= 0 ? "yes" : "missing!",
            filter_process ? filter_process : "(none)",
            filter_pid, rows_read, rows_dropped);
        return 1;
    }

    // ── 3. Stats ──────────────────────────────────────────────────────────
    std::sort(frame_times.begin(), frame_times.end());
    const uint32_t n = static_cast<uint32_t>(frame_times.size());

    double sum = 0.0;
    for (float v : frame_times) sum += static_cast<double>(v);
    const double avg = sum / static_cast<double>(n);

    // P50, P99, P99.9 con interpolación nearest-rank (suficiente para n > 100)
    auto pct_at = [&](double p) -> double {
        const std::size_t k = static_cast<std::size_t>(p * (n - 1u));
        return static_cast<double>(frame_times[k]);
    };
    PresentMonStats s{};
    s.total_frames = n;
    s.avg_ms       = avg;
    s.p50_ms       = pct_at(0.50);
    s.p99_ms       = pct_at(0.99);
    s.p999_ms      = pct_at(0.999);
    s.max_ms       = static_cast<double>(frame_times.back());

    // Stddev
    double var_sum = 0.0;
    for (float v : frame_times) {
        const double d = static_cast<double>(v) - avg;
        var_sum += d * d;
    }
    s.stddev_ms = std::sqrt(var_sum / static_cast<double>(n));

    // Stutters (relative) = frames > 2 × avg (Master Plan §5.1).
    // Stutters (absolute) = frames > 16.67 ms (sub-60Hz threshold, platform-
    // independent — useful for cross-test comparison without "threshold shifted
    // because avg changed" noise).
    s.stutter_count    = 0u;
    s.stutter_abs_60hz = 0u;
    const float stutter_rel_threshold = static_cast<float>(2.0 * avg);
    constexpr float kSub60HzMs        = 16.67f;   // 1000 ms / 60 Hz
    for (float v : frame_times) {
        if (v > stutter_rel_threshold) ++s.stutter_count;
        if (v > kSub60HzMs)            ++s.stutter_abs_60hz;
    }

    const double fps_avg     = (avg > 0.0)         ? 1000.0 / avg         : 0.0;
    const double fps_1pct_low = (s.p99_ms > 0.0)   ? 1000.0 / s.p99_ms    : 0.0;
    const double fps_01_low   = (s.p999_ms > 0.0)  ? 1000.0 / s.p999_ms   : 0.0;

    // ── 4. Print summary a stdout ─────────────────────────────────────────
    std::fprintf(stdout,
        "\n-- PresentMon import summary -------------------------\n"
        "  Input:        %s\n"
        "  Filter:       process='%s' pid=%u\n"
        "  Rows read:    %u  (kept: %u, dropped frames: %u)\n"
        "  -\n"
        "  Total frames:        %u\n"
        "  Avg FPS:             %.1f  (FT avg = %.2f ms)\n"
        "  1%% low FPS:          %.1f  (FT P99 = %.2f ms)\n"
        "  0.1%% low FPS:        %.1f  (FT P99.9 = %.2f ms)\n"
        "  Median FT:           %.2f ms (P50)\n"
        "  Max FT:              %.2f ms\n"
        "  Std Dev:             %.2f ms\n"
        "  Stutters > 2x avg:   %u  (relative threshold = %.2f ms)\n"
        "  Stutters > 16.67 ms: %u  (absolute sub-60Hz hitches)\n"
        "------------------------------------------------------\n\n",
        pm_csv,
        filter_process ? filter_process : "(any)",
        filter_pid,
        rows_read, rows_kept, rows_dropped,
        s.total_frames,
        fps_avg, s.avg_ms,
        fps_1pct_low, s.p99_ms,
        fps_01_low, s.p999_ms,
        s.p50_ms,
        s.max_ms,
        s.stddev_ms,
        s.stutter_count, static_cast<double>(stutter_rel_threshold),
        s.stutter_abs_60hz);

    // ── 5. Write ayama-bench-format CSV ───────────────────────────────────
    if (output) {
        std::FILE* out = std::fopen(output, "w");
        if (!out) {
            std::fprintf(stderr, "Error: cannot write to '%s'\n", output);
            return 1;
        }
        std::fprintf(out,
            "# ayama-cli presentmon-import output\n"
            "# source=%s\n"
            "# filter_process=%s\n"
            "# filter_pid=%u\n"
            "metric,value\n"
            "total_frames,%u\n"
            "avg_ms,%.4f\n"
            "p50_ms,%.4f\n"
            "p99_ms,%.4f\n"
            "p999_ms,%.4f\n"
            "max_ms,%.4f\n"
            "variance_ms,%.4f\n"
            "stutter_count,%u\n"
            "stutter_abs_60hz,%u\n",
            pm_csv,
            filter_process ? filter_process : "(any)",
            filter_pid,
            s.total_frames,
            s.avg_ms, s.p50_ms, s.p99_ms, s.p999_ms, s.max_ms,
            s.stddev_ms, s.stutter_count, s.stutter_abs_60hz);
        std::fclose(out);
        std::fprintf(stdout, "Written: %s\n", output);
        std::fprintf(stdout,
            "Next:    ayama-cli bench diff <baseline.csv> <treated.csv>\n");
    }
    return 0;
}

static void print_help()
{
    std::fprintf(stdout,
        "ayama-cli v0.2 — Ayama command-line interface\n\n"
        "Usage: ayama-cli <command> [args...]\n\n"
        "Commands:\n"
        "  status                                Show agent connection status\n"
        "  targets                               List currently observed processes\n"
        "  actions                               Show recent action history\n"
        "  pause                                 Revert all active actions and suspend new applies\n"
        "  resume                                Re-enable policy application (after pause)\n"
        "  revert-all                            Revert all active actions (does NOT suspend)\n"
        "  bench diff <a> <b>                    Compare two CSV files (ayama-bench / presentmon-import format)\n"
        "  bench multi --baseline <f> --treated <f>  Multi-run statistical aggregate (95%% CI + significance)\n"
        "      Repeat --baseline and --treated as needed (min 2 each, recommend 3+).\n"
        "  presentmon-import <pm.csv>            Convert PresentMon CSV → ayama-bench CSV\n"
        "      [--process <name>]                  Filter rows by Application name (case-insensitive prefix)\n"
        "      [--pid <uint>]                      Filter rows by ProcessID\n"
        "      [--output <file>]                   Output path (default: <pm>.ayama.csv)\n"
        "  memory list                           Show per-game learned entries\n"
        "  memory clear                          Delete all per-game memory (prompts)\n"
        "  memory clear-bad                      Remove all bad-list entries\n"
        "  memory clear-audit                    Delete the binary audit log (audit.bin)\n"
        "  help                                  Show this message\n\n"
        "Empirical test workflow:\n"
        "  1) Capture baseline with PresentMon:\n"
        "       PresentMon.exe --process_name javaw.exe --output_file baseline.pm.csv\n"
        "  2) Enable Ayama policy (run ayama-agent as Administrator)\n"
        "  3) Capture treated:\n"
        "       PresentMon.exe --process_name javaw.exe --output_file treated.pm.csv\n"
        "  4) Convert both:\n"
        "       ayama-cli presentmon-import baseline.pm.csv --process javaw.exe --output baseline.csv\n"
        "       ayama-cli presentmon-import treated.pm.csv  --process javaw.exe --output treated.csv\n"
        "  5) Diff:\n"
        "       ayama-cli bench diff baseline.csv treated.csv\n");
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char** argv)
{
    if (argc < 2) { print_help(); return 0; }

    const char* cmd = argv[1];

    if (std::strcmp(cmd, "status")  == 0) return cmd_status();
    if (std::strcmp(cmd, "targets") == 0) return cmd_targets();
    if (std::strcmp(cmd, "actions") == 0) return cmd_actions();

    // IPC commands to control the agent at runtime.
    if (std::strcmp(cmd, "pause")      == 0) return cmd_pause();
    if (std::strcmp(cmd, "resume")     == 0) return cmd_resume();
    if (std::strcmp(cmd, "revert-all") == 0) return cmd_revert_all();

    if (std::strcmp(cmd, "bench") == 0) {
        if (argc >= 5 && std::strcmp(argv[2], "diff") == 0) {
            return cmd_bench_diff(argv[3], argv[4]);
        }
        if (argc >= 7 && std::strcmp(argv[2], "multi") == 0) {
            // Format: bench multi --baseline f1.csv --baseline f2.csv --treated f3.csv --treated f4.csv ...
            std::vector<const char*> baselines, treateds;
            for (int i = 3; i < argc - 1; ++i) {
                if (std::strcmp(argv[i], "--baseline") == 0) {
                    baselines.push_back(argv[++i]);
                } else if (std::strcmp(argv[i], "--treated") == 0) {
                    treateds.push_back(argv[++i]);
                }
            }
            return cmd_bench_multi(baselines, treateds);
        }
        std::fprintf(stderr,
            "Usage:\n"
            "  ayama-cli bench diff <baseline.csv> <treated.csv>\n"
            "  ayama-cli bench multi --baseline <f1.csv> [--baseline <f2.csv> ...]\n"
            "                       --treated  <f1.csv> [--treated  <f2.csv> ...]\n"
            "                       (min 2 of each)\n");
        return 1;
    }

    if (std::strcmp(cmd, "presentmon-import") == 0) {
        if (argc < 3) {
            std::fprintf(stderr,
                "Usage: ayama-cli presentmon-import <pm.csv> "
                "[--process <name>] [--pid <uint>] [--output <file>]\n");
            return 1;
        }
        const char* pm_csv         = argv[2];
        const char* filter_process = nullptr;
        uint32_t    filter_pid     = 0u;
        const char* output         = nullptr;
        char        default_out[512]{};

        for (int i = 3; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--process") == 0) filter_process = argv[++i];
            else if (std::strcmp(argv[i], "--pid") == 0)
                filter_pid = static_cast<uint32_t>(std::atoi(argv[++i]));
            else if (std::strcmp(argv[i], "--output") == 0) output = argv[++i];
        }
        if (!output) {
            std::snprintf(default_out, sizeof(default_out), "%s.ayama.csv", pm_csv);
            output = default_out;
        }
        return cmd_presentmon_import(pm_csv, filter_process, filter_pid, output);
    }

    if (std::strcmp(cmd, "memory") == 0 && argc >= 3) {
        if (std::strcmp(argv[2], "list")        == 0) return cmd_memory_list();
        if (std::strcmp(argv[2], "clear")       == 0) return cmd_memory_clear();
        if (std::strcmp(argv[2], "clear-bad")   == 0) return cmd_memory_clear_bad();
        if (std::strcmp(argv[2], "clear-audit") == 0) return cmd_memory_clear_audit();
        std::fprintf(stderr, "Unknown memory command: %s\n", argv[2]);
        return 1;
    }

    if (std::strcmp(cmd, "help")   == 0 ||
        std::strcmp(cmd, "--help") == 0 ||
        std::strcmp(cmd, "-h")     == 0)
    {
        print_help();
        return 0;
    }

    std::fprintf(stderr, "Unknown command: '%s'\n\n", cmd);
    print_help();
    return 1;
}
// Made with my soul - Swately <3
