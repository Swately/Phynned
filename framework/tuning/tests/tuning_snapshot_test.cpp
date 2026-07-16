// framework/tuning/tests/tuning_snapshot_test.cpp
// TuningSnapshot persistence + rollback tests — Phase 4 restored-feature.
//
// Sections:
//   §1  add_record / records()
//   §2  RAII rollback on destruction when not committed
//   §3  mark_committed suppresses RAII rollback
//   §4  save() + load() round-trip preserves records
//   §5  load() returns IoError when file is missing
//   §6  default_snapshot_path returns a non-empty path
//   §7  Move semantics preserve records + path
//
// TuningSnapshot's rollback handlers for Win* / Sysfs ops touch real OS state.
// These tests use SysfsWrite records pointing at non-existent paths — the
// rollback code does best-effort writes and silently skips missing files, so
// no actual OS state is mutated.
//
#include <phyriad/tuning/TuningSnapshot.hpp>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>

using namespace phyriad::tuning;

static int g_pass{0};
static int g_fail{0};

#define SECTION(msg) std::printf("  § %s\n", (msg))
#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (cond) { ++g_pass; }                                                \
        else {                                                                 \
            ++g_fail;                                                          \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                      \
    } while (false)

namespace fs = std::filesystem;

static fs::path scratch_path(const char* name) {
    auto p = fs::temp_directory_path() / "gma_tuning_test";
    fs::create_directories(p);
    return p / name;
}

// ── §1 add_record / records() ────────────────────────────────────────────────
static void test_add_records() {
    SECTION("Test 1: add_record + records() exposes appended entries");
    TuningSnapshot s{scratch_path("phase4_1.bin")};
    s.add_record(TuningOp::SysfsWrite, "/sys/dummy/a", "old1", "new1");
    s.add_record(TuningOp::SysfsWrite, "/sys/dummy/b", "old2", "new2");

    auto const& r = s.records();
    EXPECT(r.size() == 2u);
    EXPECT(std::string(r[0].target)    == "/sys/dummy/a");
    EXPECT(std::string(r[0].old_value) == "old1");
    EXPECT(std::string(r[1].new_value) == "new2");
    s.mark_committed();   // skip RAII rollback for this test
}

// ── §2 RAII rollback on destruction ──────────────────────────────────────────
static void test_raii_rollback() {
    SECTION("Test 2: RAII rollback runs on destruction when not committed");
    bool dtor_ran = false;
    {
        TuningSnapshot s{scratch_path("phase4_2.bin")};
        s.add_record(TuningOp::SysfsWrite,
                     "/sys/non_existent_path_xyz", "old", "new");
        EXPECT(!s.is_committed());
        dtor_ran = true;
        // s goes out of scope → rollback_all() runs in dtor
    }
    EXPECT(dtor_ran);    // just to keep the structured-binding linter happy
}

// ── §3 mark_committed suppresses rollback ────────────────────────────────────
static void test_mark_committed() {
    SECTION("Test 3: mark_committed flips is_committed + suppresses rollback");
    TuningSnapshot s{scratch_path("phase4_3.bin")};
    s.add_record(TuningOp::SysfsWrite, "/sys/dummy", "x", "y");
    EXPECT(!s.is_committed());

    s.mark_committed();
    EXPECT(s.is_committed());
}

// ── §4 save + load round-trip ────────────────────────────────────────────────
static void test_save_load_roundtrip() {
    SECTION("Test 4: save() then load() preserves all records");
    const auto p = scratch_path("phase4_4.bin");
    fs::remove(p);   // ensure clean state

    {
        TuningSnapshot s{p};
        s.add_record(TuningOp::SysfsWrite, "/sys/x/0", "0", "1");
        s.add_record(TuningOp::SysfsWrite, "/sys/x/1", "a", "b");
        auto sv = s.save();
        EXPECT(sv.has_value());
        s.mark_committed();   // suppress RAII rollback
    }

    EXPECT(fs::exists(p));

    auto loaded = TuningSnapshot::load(p);
    EXPECT(loaded.has_value());
    if (loaded) {
        auto const& r = loaded->records();
        EXPECT(r.size() == 2u);
        EXPECT(std::string(r[0].target)    == "/sys/x/0");
        EXPECT(std::string(r[1].old_value) == "a");
        loaded->mark_committed();
    }
    fs::remove(p);
}

// ── §5 load missing file → IoError ───────────────────────────────────────────
static void test_load_missing_file() {
    SECTION("Test 5: load() of a missing file returns IoError");
    auto p = scratch_path("phase4_5_does_not_exist.bin");
    fs::remove(p);
    auto r = TuningSnapshot::load(p);
    EXPECT(!r.has_value());
    if (!r) EXPECT(r.error().code == phyriad::ErrorCode::IoError);
}

// ── §6 default_snapshot_path ─────────────────────────────────────────────────
static void test_default_snapshot_path() {
    SECTION("Test 6: default_snapshot_path returns a non-empty filename");
    const auto p = default_snapshot_path();
    EXPECT(!p.empty());
    EXPECT(p.has_filename());
    // The basename should contain "tuning_snapshot" so caller is unambiguous.
    EXPECT(p.filename().string().find("tuning_snapshot") != std::string::npos);
}

// ── §7 Move semantics ────────────────────────────────────────────────────────
static void test_move_semantics() {
    SECTION("Test 7: move ctor + move assignment preserve state");
    TuningSnapshot a{scratch_path("phase4_7.bin")};
    a.add_record(TuningOp::SysfsWrite, "/sys/z", "1", "2");
    EXPECT(a.records().size() == 1u);

    TuningSnapshot b{std::move(a)};
    EXPECT(b.records().size() == 1u);
    EXPECT(std::string(b.records()[0].target) == "/sys/z");

    TuningSnapshot c{scratch_path("phase4_7c.bin")};
    c = std::move(b);
    EXPECT(c.records().size() == 1u);
    c.mark_committed();
}

// ── main ──────────────────────────────────────────────────────────────────────
int main() {
    std::printf("[tuning_snapshot_test] phyriad_tuning\n");
    std::printf("----------------------------------------------------------------\n");

    test_add_records();
    test_raii_rollback();
    test_mark_committed();
    test_save_load_roundtrip();
    test_load_missing_file();
    test_default_snapshot_path();
    test_move_semantics();

    std::printf("----------------------------------------------------------------\n");
    const int total = g_pass + g_fail;
    if (g_fail == 0)
        std::printf("[OK] %d/%d checks passed\n", g_pass, total);
    else
        std::printf("[FAIL] %d/%d checks FAILED\n", g_fail, total);
    return g_fail ? 1 : 0;
}
// Made with my soul - Swately <3
