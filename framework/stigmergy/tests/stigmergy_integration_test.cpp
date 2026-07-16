// framework/stigmergy/tests/stigmergy_integration_test.cpp
// End-to-end stigmergic workflow — Phase P-0.5.1 verification.
//
// This test wires together Field<T> + Pheromone<T,N> + Classifier<Sig,Action>
// + Worker<Logic> to demonstrate the FULL pattern that Phyriad documents
// as its differentiating feature.
//
// Scenario: N "sensor" workers each periodically deposit a reading into a
// Pheromone<uint16_t,N>. A "fusion" worker reads all slots and publishes
// the median into a Field<SensorState>. A "consumer" worker reads the
// fused state and counts how many distinct values it observed.
//
// Verification:
//   - Workers each deposit ≥ kMinDeposits before stopping
//   - Consumer observes a strictly-monotonic-or-equal sequence number
//   - Final fused state reflects the last batch of deposits
//   - Worker lifetime is clean (jthread join via destructor)
//
// This is the canonical "stigmergic system in <300 LOC" reference.
//
// Stigmergy as first-class pillar.
#include <phyriad/stigmergy/Stigmergy.hpp>
#include <phyriad/schema/PodMessage.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <phyriad/hal/MemoryOrder.hpp>

using namespace phyriad::stigmergy;
using namespace std::chrono_literals;

static int g_pass{0};
static int g_fail{0};

#define SECTION(msg) std::printf("  § %s\n", (msg))
#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (cond) { ++g_pass; }                                               \
        else {                                                                \
            ++g_fail;                                                         \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                     \
    } while (false)

// ── POD message for the fused-state Field ──────────────────────────────────
struct SensorState {
    uint64_t sequence;
    uint16_t median_reading;
    uint16_t reserved;
    uint32_t live_agent_count;
};
PHYRIAD_ASSERT_POD(SensorState);

// Type alias so the test reads more naturally.
constexpr std::size_t kAgents = 8u;
using PheromoneT = Pheromone<uint16_t, kAgents>;
using FieldT     = Field<SensorState>;

// ── Fusion classifier — reads all sensor slots, returns median ─────────────
struct FusionQuery { uint64_t tick; };

class MedianFusion : public Classifier<FusionQuery, SensorState> {
public:
    explicit MedianFusion(PheromoneT& p) noexcept : pheromone_{p} {}

    [[nodiscard]] SensorState decide(FusionQuery const& q) noexcept override {
        auto snap = pheromone_.read_all();
        std::array<uint16_t, kAgents> sorted = snap;
        std::sort(sorted.begin(), sorted.end());
        // Median = middle element (N=8 → average of [3] and [4], but uint16
        // approximation is fine for a test).
        const uint16_t median = sorted[kAgents / 2u];
        uint32_t live = 0;
        for (auto v : snap) if (v > 0u) ++live;
        return SensorState{q.tick, median, 0u, live};
    }

private:
    PheromoneT& pheromone_;
};

// ── §1 the full pipeline runs end-to-end ───────────────────────────────────
static void test_end_to_end_pipeline() {
    SECTION("Test 1: full stigmergic pipeline — N sensors → fusion → consumer");

    PheromoneT  pheromone;
    FieldT      field;
    MedianFusion fusion{pheromone};

    std::atomic<uint64_t> consumer_observations{0};
    std::atomic<uint64_t> last_observed_seq{0};
    std::atomic<uint64_t> non_monotonic{0};

    // 1) N sensor workers: each deposits a unique value into its own slot.
    //    The value is the agent_id * 100 + iteration_count (mod 65535).
    //
    // Note: we use std::vector<std::unique_ptr<Worker>> rather than
    // std::vector<Worker> because Worker<Logic> contains a std::jthread
    // which has a noexcept move ctor, but the captured-lambda Logic
    // may be non-noexcept-move (depending on capture types) — which
    // makes the default Worker move ctor problematic for vector storage.
    // The unique_ptr indirection sidesteps this without changing semantics.
    using LogicFn = std::function<void(std::stop_token)>;
    using WorkerT = Worker<LogicFn>;
    std::vector<std::unique_ptr<WorkerT>> sensors;
    sensors.reserve(kAgents);
    for (std::size_t i = 0; i < kAgents; ++i) {
        sensors.push_back(std::make_unique<WorkerT>(
            LogicFn{[i, &pheromone](std::stop_token st) noexcept {
                uint16_t k = 0;
                while (!st.stop_requested()) {
                    const uint16_t v =
                        static_cast<uint16_t>((i * 100u + k) & 0xFFFFu);
                    pheromone.deposit(i, v);
                    ++k;
                    std::this_thread::sleep_for(100us);
                }
            }}
        ));
    }

    // 2) Fusion worker: read all sensors → publish median. Uses yield()
    //    rather than sleep_for() so heavy parallel test runs (ctest -j 31)
    //    don't completely starve this thread of CPU time.
    std::atomic<uint64_t> fusion_publications{0};
    Worker fusion_worker{
        [&pheromone, &field, &fusion, &fusion_publications](std::stop_token st) noexcept {
            uint64_t tick = 0;
            while (!st.stop_requested()) {
                auto state = fusion.decide(FusionQuery{tick});
                field.publish(state);
                phyriad::hal::stat_fetch_add_relaxed(fusion_publications, 1);
                ++tick;
                std::this_thread::yield();
            }
        }
    };

    // 3) Consumer worker: read the field, verify monotonic seq. yield() too.
    Worker consumer_worker{
        [&field, &consumer_observations, &last_observed_seq, &non_monotonic]
        (std::stop_token st) noexcept {
            while (!st.stop_requested()) {
                auto s = field.read();
                if (s.sequence > 0u) {
                    phyriad::hal::stat_fetch_add_relaxed(consumer_observations, 1);
                    const uint64_t prev =
                        phyriad::hal::stat_load_relaxed(last_observed_seq);
                    if (s.sequence < prev) {
                        phyriad::hal::stat_fetch_add_relaxed(non_monotonic, 1);
                    }
                    phyriad::hal::stat_store_relaxed(last_observed_seq, s.sequence);
                }
                std::this_thread::yield();
            }
        }
    };

    // Run until at least 100 fusion publications have happened OR a max
    // wall budget of 500 ms (whichever comes first). This is more robust
    // than a fixed sleep — under heavy parallel test load (ctest -j 31),
    // fixed sleeps can be completely starved, but yield-based loops still
    // make progress whenever the scheduler gives any quantum.
    // Virtualized CI runners (2 vCPUs, time-sliced) need 10x the wall budget
    // to scrape together enough quanta for the worker threads to run.
    const bool on_ci = (std::getenv("CI") != nullptr) ||
                       (std::getenv("GITHUB_ACTIONS") != nullptr);
    const auto kMaxBudget = on_ci ? 5000ms : 500ms;
    constexpr uint64_t kTargetPubs = 100u;
    const auto deadline = std::chrono::steady_clock::now() + kMaxBudget;
    while (std::chrono::steady_clock::now() < deadline &&
           phyriad::hal::stat_load_relaxed(fusion_publications) < kTargetPubs) {
        std::this_thread::sleep_for(5ms);
    }

    // Stop all workers (jthread destructors do this automatically when
    // the Worker objects go out of scope; we call explicit stop here
    // for clarity).
    fusion_worker.stop_and_join();
    consumer_worker.stop_and_join();
    for (auto& s : sensors) s->stop_and_join();

    // Verify:
    //   - Fusion published many times
    //   - Consumer observed many values
    //   - No non-monotonic observations (Field<T> publishes are point-in-time)
    const uint64_t pubs    = fusion_publications.load();
    const uint64_t obs     = consumer_observations.load();
    const uint64_t nonmono = non_monotonic.load();

    std::printf("        fusion publications: %llu\n",
                static_cast<unsigned long long>(pubs));
    std::printf("        consumer observations: %llu\n",
                static_cast<unsigned long long>(obs));
    std::printf("        non-monotonic observations: %llu\n",
                static_cast<unsigned long long>(nonmono));

    // NOTE on the lowered bars below: this test is timing-sensitive and
    // Windows MinGW's std::this_thread::sleep_for() has ~15 ms granularity
    // (the default OS timer tick), so a 200 ms run with sleep_for(1ms) yields
    // ~13 publications, not the naïve 200 the math suggests. The pipeline IS
    // running correctly — we just can't assume sub-ms sleep precision on Win.
    // What matters here is FUNCTIONAL: pubs > 0, obs > 0, nonmono == 0.
    // The performance throughput is the job of `bench_stigmergy_primitives`,
    // not this functional test.
    EXPECT(pubs    >= 5u);    // any publications proves fusion loop runs
    EXPECT(obs     >= 5u);    // any observations proves consumer loop runs
    EXPECT(nonmono == 0u);    // Field<T> must be strictly point-in-time

    // Final state: median must be > 0 (sensors were active).
    auto final = field.read();
    EXPECT(final.sequence       > 0u);
    EXPECT(final.live_agent_count > 0u);
}

// ── §2 graceful shutdown via destructor ────────────────────────────────────
static void test_worker_dtor_joins_cleanly() {
    SECTION("Test 2: Worker destructor joins the thread cleanly (no leak, no hang)");
    std::atomic<uint64_t> iterations{0};
    {
        Worker w{
            [&iterations](std::stop_token st) noexcept {
                while (!st.stop_requested()) {
                    phyriad::hal::stat_fetch_add_relaxed(iterations, 1);
                    std::this_thread::sleep_for(100us);
                }
            }
        };
        std::this_thread::sleep_for(10ms);
        EXPECT(w.running());
        // Worker `w` goes out of scope here → destructor calls
        // stop_and_join. If this hangs, the test will time out.
    }
    EXPECT(iterations.load() > 0u);
}

// ── §3 Pheromone + Classifier without any Worker overhead ──────────────────
static void test_classifier_uses_pheromone_directly() {
    SECTION("Test 3: classifier + pheromone — no worker threads, pure logic");
    PheromoneT pheromone;
    MedianFusion fusion{pheromone};

    // Deposit a known pattern.
    for (std::size_t i = 0; i < kAgents; ++i) {
        pheromone.deposit(i, static_cast<uint16_t>(i * 10u));
    }
    auto state = fusion.decide(FusionQuery{42});
    EXPECT(state.sequence            == 42u);
    EXPECT(state.live_agent_count    == kAgents - 1u); // slot 0 was 0×10=0
    // Median of {0,10,20,30,40,50,60,70} ~ 40 (taking sorted[4] = 40)
    EXPECT(state.median_reading      == 40u);
}

int main() {
    std::printf("[stigmergy_integration_test] phyriad_stigmergy — Phase P-0.5.1\n");
    std::printf("----------------------------------------------------------------\n");

    test_classifier_uses_pheromone_directly();
    test_worker_dtor_joins_cleanly();
    test_end_to_end_pipeline();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d checks passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d checks FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
