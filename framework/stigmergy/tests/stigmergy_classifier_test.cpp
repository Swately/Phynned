// framework/stigmergy/tests/stigmergy_classifier_test.cpp
// Classifier<Sig, Action> unit tests — Phase P-0.5.1 verification.
//
// Sections:
//   §1   abstract class — can't instantiate directly (compile-time check)
//   §2   concrete subclass dispatches decide() correctly
//   §3   classifier holding a Field<T> reads it on every decide
//   §4   classifier holding a Pheromone<T,N> reads all slots
//   §5   stateful classifier (mutable counter) — counts decisions
//   §6   concurrent: multiple threads invoke decide() concurrently
//
// Stigmergy as first-class pillar.
#include <phyriad/stigmergy/Classifier.hpp>
#include <phyriad/stigmergy/Field.hpp>
#include <phyriad/stigmergy/Pheromone.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <type_traits>
#include <vector>
#include <phyriad/hal/MemoryOrder.hpp>

using namespace phyriad::stigmergy;
using phyriad::schema::SampleTick;

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

// ── §1 abstract-class compile-time guarantee ────────────────────────────────
// We expect Classifier<int,int> NOT to be default-constructible (it has a
// pure virtual). Verify via type traits.
static_assert(!std::is_constructible_v<Classifier<int, int>>,
    "Classifier must be abstract (has pure virtual decide())");
static_assert(std::is_abstract_v<Classifier<int, int>>,
    "Classifier must be abstract");

// ── §2 concrete subclass + simple decide ────────────────────────────────────
struct EchoSignal { uint32_t v; };
struct EchoAction { uint32_t v; };

class EchoClassifier : public Classifier<EchoSignal, EchoAction> {
public:
    [[nodiscard]] EchoAction decide(EchoSignal const& s) noexcept override {
        return EchoAction{s.v};
    }
};

static void test_concrete_echo() {
    SECTION("Test 2: concrete subclass — decide() returns identity transform");
    EchoClassifier c;
    auto a = c.decide(EchoSignal{42u});
    EXPECT(a.v == 42u);
}

// ── §3 classifier reading Field<T> ──────────────────────────────────────────
struct PriceQuery { uint32_t pad; };
struct PriceQuoteAction { uint64_t latest_price; };

class FieldReadingClassifier
    : public Classifier<PriceQuery, PriceQuoteAction> {
public:
    explicit FieldReadingClassifier(Field<SampleTick> const& f) noexcept
        : field_{f} {}

    [[nodiscard]] PriceQuoteAction
    decide(PriceQuery const&) noexcept override {
        const auto t = field_.read();
        return PriceQuoteAction{t.price};
    }

private:
    Field<SampleTick> const& field_;
};

static void test_classifier_reads_field() {
    SECTION("Test 3: classifier holding Field<T> reads it per decide()");
    Field<SampleTick> field;
    FieldReadingClassifier cls{field};

    SampleTick t1{};  t1.price = 100u;  field.publish(t1);
    auto a1 = cls.decide(PriceQuery{});
    EXPECT(a1.latest_price == 100u);

    SampleTick t2{};  t2.price = 250u;  field.publish(t2);
    auto a2 = cls.decide(PriceQuery{});
    EXPECT(a2.latest_price == 250u);
}

// ── §4 classifier reading Pheromone<T,N> ────────────────────────────────────
struct RouteQuery { uint32_t pad; };
struct RouteAction { uint32_t target_worker; uint8_t min_fill_pct; };

class FillBasedRouter
    : public Classifier<RouteQuery, RouteAction> {
public:
    explicit FillBasedRouter(Pheromone<uint8_t, 8>& fill) noexcept
        : fill_{fill} {}

    [[nodiscard]] RouteAction
    decide(RouteQuery const&) noexcept override {
        auto snap = fill_.read_all();
        uint32_t best = 0;
        uint8_t  best_fill = 255u;
        for (uint32_t i = 0; i < snap.size(); ++i) {
            if (snap[i] < best_fill) {
                best_fill = snap[i];
                best = i;
            }
        }
        return RouteAction{best, best_fill};
    }

private:
    Pheromone<uint8_t, 8>& fill_;
};

static void test_classifier_reads_pheromone() {
    SECTION("Test 4: FillBasedRouter — routes to least-loaded slot");
    Pheromone<uint8_t, 8> fill;
    FillBasedRouter router{fill};

    // All zero → routes to slot 0, fill=0.
    auto a0 = router.decide(RouteQuery{});
    EXPECT(a0.target_worker == 0u);
    EXPECT(a0.min_fill_pct  == 0u);

    // Slot 5 is uniquely empty.
    fill.deposit(0, 90u);
    fill.deposit(1, 80u);
    fill.deposit(2, 70u);
    fill.deposit(3, 60u);
    fill.deposit(4, 50u);
    fill.deposit(5, 10u);   // ← lowest
    fill.deposit(6, 70u);
    fill.deposit(7, 80u);
    auto a1 = router.decide(RouteQuery{});
    EXPECT(a1.target_worker == 5u);
    EXPECT(a1.min_fill_pct  == 10u);
}

// ── §5 stateful classifier (mutable counter) ────────────────────────────────
class CountingClassifier : public Classifier<EchoSignal, EchoAction> {
public:
    [[nodiscard]] EchoAction decide(EchoSignal const& s) noexcept override {
        phyriad::hal::stat_fetch_add_relaxed(count_, 1);
        return EchoAction{s.v};
    }
    [[nodiscard]] uint64_t count() const noexcept {
        return phyriad::hal::stat_load_relaxed(count_);
    }
private:
    mutable std::atomic<uint64_t> count_{0};
};

static void test_stateful_classifier() {
    SECTION("Test 5: stateful classifier — counts decide() invocations");
    CountingClassifier c;
    for (uint32_t i = 0; i < 1000u; ++i) (void)c.decide(EchoSignal{i});
    EXPECT(c.count() == 1000u);
}

// ── §6 concurrent decide() ──────────────────────────────────────────────────
static void test_concurrent_decide() {
    SECTION("Test 6: 4 threads × 100k decide() each — counter exact");
    CountingClassifier c;
    constexpr int kThreads = 4;
    constexpr uint64_t kPerThread = 100'000u;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&c]() noexcept {
            for (uint64_t i = 0; i < kPerThread; ++i)
                (void)c.decide(EchoSignal{static_cast<uint32_t>(i)});
        });
    }
    for (auto& t : ts) t.join();
    EXPECT(c.count() == kThreads * kPerThread);
}

int main() {
    std::printf("[stigmergy_classifier_test] phyriad_stigmergy — Phase P-0.5.1\n");
    std::printf("----------------------------------------------------------------\n");

    test_concrete_echo();
    test_classifier_reads_field();
    test_classifier_reads_pheromone();
    test_stateful_classifier();
    test_concurrent_decide();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d checks passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d checks FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
