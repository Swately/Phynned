// tools/phynned-bench/workload.hpp
// Phynned-bench synthetic workload — simulates a CPU-bound game loop.
//
// Design goals:
//   - Stresses CPU scheduler: main thread + N worker threads doing bursts.
//   - Produces measurable frametime + migration metrics.
//   - Runs without GPU, DX, or any external library.
//   - Deterministic enough for A/B comparison (same seed → same ops).
//
// Workload structure:
//   main thread: hot matrix multiply loop (8 MB cache working set).
//   worker threads: periodic bursts of std::sort + memcpy (simulate physics/AI).
//
// Threading: 1 main thread + kWorkerCount worker threads.
// Resource:  ~8 MB + (worker_count × 1 MB) working set.
// Privilege: None.
//
// §Phynned Master Plan §5.4
#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include <array>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <phyriad/hal/MemoryOrder.hpp>

namespace phynned_bench {

// ── Config ────────────────────────────────────────────────────────────────
static constexpr uint32_t kMatrixDim    = 512u;   // 512×512 float = 1 MB per matrix
static constexpr uint32_t kNumMatrices  = 8u;     // total working set ≈ 8 MB
static constexpr uint32_t kWorkerCount  = 4u;     // physics/AI sim threads
static constexpr uint32_t kWorkerSortN  = 65536u; // sort array size per burst
static constexpr uint32_t kTargetFps    = 60u;    // simulated frame budget
static constexpr uint32_t kFrameBudgetUs = 1'000'000u / kTargetFps;  // 16667 µs

// ── FrameSample ──────────────────────────────────────────────────────────
struct FrameSample {
    float frame_time_ms;   ///< Duration of this "frame" in milliseconds.
    float cpu_temp_bogus;  ///< Placeholder (not measured here).
};

// ── WorkloadResult ───────────────────────────────────────────────────────
struct WorkloadResult {
    float avg_ms;          ///< Mean frame time.
    float p99_ms;          ///< 99th-percentile frame time.
    float p999_ms;         ///< 99.9th-percentile frame time.
    float variance_ms;     ///< Variance (std dev).
    uint32_t stutter_count; ///< Frames > 2 × avg.
    uint32_t total_frames;
    uint32_t migrations_estimate; ///< Placeholder.
};

// ── Matrices (large working set for cache pressure) ──────────────────────
alignas(64) static float g_mat_a[kNumMatrices][kMatrixDim * kMatrixDim];
alignas(64) static float g_mat_b[kNumMatrices][kMatrixDim * kMatrixDim];
alignas(64) static float g_mat_c[kNumMatrices][kMatrixDim * kMatrixDim];

/// Initialise matrices with pseudo-random values.
inline void init_matrices(uint32_t seed) noexcept
{
    for (uint32_t m = 0u; m < kNumMatrices; ++m) {
        for (uint32_t i = 0u; i < kMatrixDim * kMatrixDim; ++i) {
            seed ^= seed << 13u; seed ^= seed >> 17u; seed ^= seed << 5u;
            g_mat_a[m][i] = static_cast<float>(seed & 0xFFFFu) / 65535.0f;
            seed ^= seed << 13u; seed ^= seed >> 17u; seed ^= seed << 5u;
            g_mat_b[m][i] = static_cast<float>(seed & 0xFFFFu) / 65535.0f;
        }
    }
}

/// Multiply two small sub-tiles to create cache pressure (simplified).
inline void matrix_work(uint32_t mat_idx) noexcept
{
    const float* __restrict a = g_mat_a[mat_idx % kNumMatrices];
    const float* __restrict b = g_mat_b[mat_idx % kNumMatrices];
    float*       __restrict c = g_mat_c[mat_idx % kNumMatrices];

    // Simplified: dot-product stripes (not full O(N³) — tuned to ~4 ms).
    constexpr uint32_t kStride = 64u;
    for (uint32_t row = 0u; row < kMatrixDim; row += kStride) {
        for (uint32_t col = 0u; col < kMatrixDim; col += kStride) {
            for (uint32_t k = 0u; k < kStride; ++k) {
                for (uint32_t j = col; j < col + kStride; ++j) {
                    c[row * kMatrixDim + j] += a[row * kMatrixDim + k] *
                                              b[k  * kMatrixDim + j];
                }
            }
        }
    }
}

// ── Worker burst ─────────────────────────────────────────────────────────
alignas(64) static float g_worker_buf[kWorkerCount][kWorkerSortN];

inline void worker_burst(uint32_t worker_id, uint32_t seed) noexcept
{
    float* buf = g_worker_buf[worker_id % kWorkerCount];
    // Fill with pseudo-random data.
    for (uint32_t i = 0u; i < kWorkerSortN; ++i) {
        seed ^= seed << 13u; seed ^= seed >> 17u; seed ^= seed << 5u;
        buf[i] = static_cast<float>(seed & 0xFFFFFFu);
    }
    // Sort — cache-unfriendly, creates migrations on heterogeneous systems.
    std::sort(buf, buf + kWorkerSortN);
    // Memcpy back — ensures dirty cache lines.
    std::memcpy(g_worker_buf[(worker_id + 1u) % kWorkerCount], buf,
                kWorkerSortN * sizeof(float));
}

// ── Workload runner ───────────────────────────────────────────────────────
class WorkloadRunner {
public:
    explicit WorkloadRunner(uint32_t duration_ms,
                            uint32_t seed = 0x1337BEEFu) noexcept
        : duration_ms_(duration_ms), seed_(seed)
    {}

    WorkloadResult run() noexcept {
        init_matrices(seed_);

        std::vector<FrameSample> samples;
        samples.reserve(duration_ms_ / (kFrameBudgetUs / 1000u) + 64u);

        // Launch worker threads.
        std::array<std::thread, kWorkerCount> workers{};
        std::atomic<bool> stop_workers{false};
        std::atomic<uint32_t> worker_tick{0u};

        for (uint32_t w = 0u; w < kWorkerCount; ++w) {
            workers[w] = std::thread([w, &stop_workers, &worker_tick, this]() noexcept {
                uint32_t local_seed = seed_ ^ (w * 0x9E3779B9u);
                while (!phyriad::hal::seq_load_acquire(stop_workers)) {
                    // Burst every ~8 ms.
                    std::this_thread::sleep_for(std::chrono::milliseconds(8));
                    worker_burst(w, local_seed ^ worker_tick.load());
                    ++local_seed;
                }
            });
        }

        // Main loop — simulate frames.
        const auto wall_start = std::chrono::steady_clock::now();
        const auto wall_end   = wall_start + std::chrono::milliseconds(duration_ms_);

        uint32_t frame = 0u;
        while (std::chrono::steady_clock::now() < wall_end) {
            const auto t0 = std::chrono::steady_clock::now();

            // Do work for this frame.
            matrix_work(frame % kNumMatrices);

            const auto t1 = std::chrono::steady_clock::now();
            const float ft_ms = static_cast<float>(
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
            ) / 1000.0f;

            samples.push_back(FrameSample{ft_ms, 0.0f});
            ++frame;
            ++worker_tick;

            // Sleep for remaining frame budget.
            const auto target_end = t0 + std::chrono::microseconds(kFrameBudgetUs);
            if (std::chrono::steady_clock::now() < target_end) {
                std::this_thread::sleep_until(target_end);
            }
        }

        phyriad::hal::seq_store_release(stop_workers, true);
        for (auto& w : workers) { if (w.joinable()) w.join(); }

        return compute_result(samples);
    }

private:
    uint32_t duration_ms_;
    uint32_t seed_;

    static WorkloadResult compute_result(std::vector<FrameSample>& s) noexcept {
        WorkloadResult r{};
        r.total_frames = static_cast<uint32_t>(s.size());
        if (r.total_frames == 0u) return r;

        // Mean.
        double sum = 0.0;
        for (const auto& f : s) sum += f.frame_time_ms;
        r.avg_ms = static_cast<float>(sum / r.total_frames);

        // Variance (std dev).
        double sq = 0.0;
        for (const auto& f : s) {
            const double d = f.frame_time_ms - r.avg_ms;
            sq += d * d;
        }
        r.variance_ms = static_cast<float>(std::sqrt(sq / r.total_frames));

        // Stutters.
        for (const auto& f : s) {
            if (f.frame_time_ms > r.avg_ms * 2.0f) ++r.stutter_count;
        }

        // P99, P999.
        std::sort(s.begin(), s.end(), [](const FrameSample& a, const FrameSample& b){
            return a.frame_time_ms < b.frame_time_ms;
        });
        r.p99_ms  = s[static_cast<size_t>(r.total_frames * 0.99f)].frame_time_ms;
        r.p999_ms = s[static_cast<size_t>(r.total_frames * 0.999f)].frame_time_ms;

        return r;
    }
};

} // namespace phynned_bench
// Made with my soul - Swately <3
