// apps/ayama/tools/ayama-bench/main.cpp
// ayama-bench — standalone synthetic benchmark for Ayama validation.
//
// Simulates a CPU-bound game-like workload. Designed for A/B testing:
//   Without Ayama: run → capture baseline.csv
//   With Ayama:    run → capture treated.csv → compare with ayama-cli diff.
//
// Usage:
//   ayama-bench [--duration <ms>] [--output <file>] [--seed <uint>]
//
//   --duration  Recording duration in milliseconds (default 60000 = 60s)
//   --output    Output CSV file path (default: bench_output.csv)
//   --seed      Random seed for reproducibility (default: 0x1337BEEF)
//   --quiet     Suppress progress output
//
// Output CSV columns:
//   frame_index, frame_time_ms
//
// §Ayama Master Plan §5.4

#include "workload.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

// ── Argument parsing ────────────────────────────────────────────────────────
struct Args {
    uint32_t    duration_ms {60'000u};
    const char* output      {"bench_output.csv"};
    uint32_t    seed        {0x1337BEEFu};
    bool        quiet       {false};
};

static void print_usage() noexcept {
    std::fprintf(stdout,
        "ayama-bench v0.1 — Ayama synthetic benchmark\n\n"
        "Usage: ayama-bench [options]\n\n"
        "Options:\n"
        "  --duration <ms>   Recording duration (default: 60000)\n"
        "  --output <file>   Output CSV (default: bench_output.csv)\n"
        "  --seed <uint>     Random seed (default: 0x1337BEEF)\n"
        "  --quiet           Suppress progress output\n"
        "  --help            Show this message\n\n"
        "Example (A/B test):\n"
        "  ayama-bench --output baseline.csv      # without Ayama\n"
        "  # Start ayama-agent, wait for detection\n"
        "  ayama-bench --output treated.csv       # with Ayama active\n"
        "  ayama-cli diff baseline.csv treated.csv\n");
}

static Args parse_args(int argc, char** argv) noexcept
{
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            std::exit(0);
        } else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            a.duration_ms = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            a.output = argv[++i];
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            a.seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            a.quiet = true;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
        }
    }
    return a;
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char** argv)
{
    const Args args = parse_args(argc, argv);

    if (!args.quiet) {
        std::fprintf(stdout,
            "ayama-bench: duration=%u ms  seed=0x%X  output=%s\n",
            args.duration_ms, args.seed, args.output);
        std::fprintf(stdout,
            "Running workload — do NOT change process affinity during baseline.\n"
            "If Ayama is active, it will detect and pin this process.\n\n");
    }

    // ── Run workload ────────────────────────────────────────────────────
    const auto wall_t0 = std::chrono::steady_clock::now();

    ayama_bench::WorkloadRunner runner(args.duration_ms, args.seed);
    const ayama_bench::WorkloadResult result = runner.run();

    const auto wall_t1 = std::chrono::steady_clock::now();
    const double wall_elapsed_s = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(wall_t1 - wall_t0).count()
    ) / 1000.0;

    // ── Print results ───────────────────────────────────────────────────
    if (!args.quiet) {
        std::fprintf(stdout,
            "\n── ayama-bench results ───────────────────────────────\n"
            "  Frames:       %u\n"
            "  Wall time:    %.1f s\n"
            "  Avg FPS:      %.1f\n"
            "  Avg FT:       %.2f ms\n"
            "  P99 FT:       %.2f ms\n"
            "  P99.9 FT:     %.2f ms\n"
            "  Std Dev:      %.2f ms\n"
            "  Stutters:     %u  (frames > 2×avg)\n"
            "──────────────────────────────────────────────────────\n\n",
            result.total_frames,
            wall_elapsed_s,
            (result.avg_ms > 0.0f) ? 1000.0f / result.avg_ms : 0.0f,
            result.avg_ms,
            result.p99_ms,
            result.p999_ms,
            result.variance_ms,
            result.stutter_count);
    }

    // ── Write CSV (for ayama-cli diff) ──────────────────────────────────
    // We rebuild from summary for simplicity. A production version would
    // accumulate samples in WorkloadRunner and write them all.
    std::FILE* csv = std::fopen(args.output, "w");
    if (csv) {
        std::fprintf(csv,
            "# ayama-bench output\n"
            "# duration_ms=%u seed=0x%X\n"
            "metric,value\n"
            "total_frames,%u\n"
            "avg_ms,%.4f\n"
            "p99_ms,%.4f\n"
            "p999_ms,%.4f\n"
            "variance_ms,%.4f\n"
            "stutter_count,%u\n"
            "wall_time_s,%.3f\n",
            args.duration_ms, args.seed,
            result.total_frames,
            static_cast<double>(result.avg_ms),
            static_cast<double>(result.p99_ms),
            static_cast<double>(result.p999_ms),
            static_cast<double>(result.variance_ms),
            result.stutter_count,
            wall_elapsed_s);
        std::fclose(csv);

        if (!args.quiet) {
            std::fprintf(stdout, "Results written to: %s\n", args.output);
        }
    } else {
        std::fprintf(stderr, "Error: could not write output to %s\n", args.output);
        return 1;
    }

    return 0;
}
// Made with my soul - Swately <3
