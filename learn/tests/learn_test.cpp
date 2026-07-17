// learn/tests/learn_test.cpp
// Test T8 — Learn-and-persist (PerGameMemory roundtrip).
//
// Verifies:
//   T8.1  LearnedEntry POD properties (192B, trivially copyable).
//   T8.2  BadEntry POD properties (128B).
//   T8.3  upsert() + find() — basic roundtrip in-memory.
//   T8.4  save() + load() — full TOML persistence roundtrip.
//   T8.5  Duplicate upsert updates the existing entry (no duplicate).
//   T8.6  mark_bad() / is_bad() / clear_bad().
//   T8.7  needs_revalidation(): user_locked=true → never stale.
//   T8.8  needs_revalidation(): sample_count=0 → stale.
//   T8.9  expire_stale_entries() zeroes stale sample_count while keeping key.
//   T8.10 remove() evicts entry, find() returns nullptr.
//
// No admin required. Writes to a temp file in the system temp directory.
//
// §9.3, phynned::learn

#include <phynned/learn/PerGameMemory.hpp>
#include <phynned/learn/LearnedEntry.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <type_traits>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/// Build a temp file path for the test. Caller must remove it afterward.
static bool make_temp_path(char* out, uint32_t max_len) noexcept
{
#ifdef _WIN32
    char tmp_dir[MAX_PATH]{};
    if (!GetTempPathA(MAX_PATH, tmp_dir)) return false;
    std::snprintf(out, max_len,
                  "%s\\phynned_learn_test_%u.toml",
                  tmp_dir,
                  static_cast<unsigned>(GetCurrentProcessId()));
#else
    std::snprintf(out, max_len,
                  "/tmp/phynned_learn_test_%u.toml",
                  static_cast<unsigned>(getpid()));
#endif
    return true;
}

int main()
{
    using namespace phynned::learn;

    // ── T8.1: LearnedEntry POD ────────────────────────────────────────────────
    {
        static_assert(sizeof(LearnedEntry) == 192u,
            "LearnedEntry must be 192B");
        static_assert(std::is_trivially_copyable_v<LearnedEntry>);
        static_assert(std::is_standard_layout_v<LearnedEntry>);
        std::printf("[OK] T8.1  LearnedEntry: 192B, trivially copyable\n");
    }

    // ── T8.2: BadEntry POD ────────────────────────────────────────────────────
    {
        static_assert(sizeof(BadEntry) == 128u,
            "BadEntry must be 128B");
        std::printf("[OK] T8.2  BadEntry: 128B\n");
    }

    // ── Setup: generate hardware ID ───────────────────────────────────────────
    PerGameMemory mem;
    mem.generate_hardware_id();
    assert(mem.hardware_id()[0] != '\0');
    std::printf("     hardware_id: '%s'\n", mem.hardware_id());

    // ── T8.3: upsert() + find() roundtrip (in-memory) ────────────────────────
    {
        LearnedEntry e{};
        std::strncpy(e.exe, "Cyberpunk2077.exe", sizeof(e.exe) - 1);
        std::strncpy(e.hardware_id, mem.hardware_id(), sizeof(e.hardware_id) - 1);
        std::strncpy(e.best_action, "pin_v_cache_ccd", sizeof(e.best_action) - 1);
        e.best_core_mask  = 0x00FFull;
        e.improvement_pct = 12.4f;
        e.sample_count    = 8u;
        std::strncpy(e.last_validated, "2026-05-19T14:22:00Z",
                     sizeof(e.last_validated) - 1);
        e.user_locked = false;

        mem.upsert(e);
        assert(mem.count() == 1u);

        const LearnedEntry* found = mem.find("Cyberpunk2077.exe");
        assert(found != nullptr);
        assert(std::strcmp(found->exe, "Cyberpunk2077.exe") == 0);
        assert(found->best_core_mask  == 0x00FFull);
        assert(found->sample_count    == 8u);
        assert(found->improvement_pct > 12.0f && found->improvement_pct < 13.0f);
        std::printf("[OK] T8.3  upsert+find: Cyberpunk2077.exe found, core_mask=0x%llx\n",
                    static_cast<unsigned long long>(found->best_core_mask));
    }

    // ── T8.4: save() + load() TOML roundtrip ─────────────────────────────────
    {
        char path[512]{};
        assert(make_temp_path(path, sizeof(path)));

        const auto save_r = mem.save(path);
        assert(save_r.has_value() && "save() must succeed");

        // Verify file is non-empty.
        std::FILE* f = std::fopen(path, "r");
        assert(f != nullptr);
        std::fseek(f, 0, SEEK_END);
        const long fsize = std::ftell(f);
        std::fclose(f);
        assert(fsize > 0);

        // Load into a fresh instance.
        PerGameMemory mem2;
        mem2.generate_hardware_id();
        const auto load_r = mem2.load(path);
        assert(load_r.has_value() && "load() must succeed");
        assert(mem2.count() == 1u);

        const LearnedEntry* e2 = mem2.find("Cyberpunk2077.exe");
        assert(e2 != nullptr);
        assert(std::strcmp(e2->exe, "Cyberpunk2077.exe") == 0);
        assert(std::strcmp(e2->best_action, "pin_v_cache_ccd") == 0);
        assert(e2->sample_count == 8u);

        std::remove(path);
        std::printf("[OK] T8.4  save()+load() roundtrip: %ld bytes, entry preserved\n",
                    fsize);
    }

    // ── T8.5: Duplicate upsert updates, doesn't duplicate ────────────────────
    {
        LearnedEntry updated{};
        std::strncpy(updated.exe, "Cyberpunk2077.exe", sizeof(updated.exe) - 1);
        std::strncpy(updated.hardware_id, mem.hardware_id(),
                     sizeof(updated.hardware_id) - 1);
        std::strncpy(updated.best_action, "pin_v_cache_ccd",
                     sizeof(updated.best_action) - 1);
        updated.best_core_mask  = 0xFFFFull;   // new mask
        updated.improvement_pct = 15.0f;
        updated.sample_count    = 12u;
        updated.user_locked     = false;

        mem.upsert(updated);
        assert(mem.count() == 1u);  // still 1, not 2

        const LearnedEntry* found = mem.find("Cyberpunk2077.exe");
        assert(found != nullptr);
        assert(found->sample_count    == 12u);
        assert(found->best_core_mask  == 0xFFFFull);
        std::printf("[OK] T8.5  Duplicate upsert: count still 1, updated values\n");
    }

    // ── T8.6: mark_bad / is_bad / clear_bad ──────────────────────────────────
    {
        mem.mark_bad("Cyberpunk2077.exe", "regression_detected");
        assert(mem.is_bad("Cyberpunk2077.exe"));
        assert(mem.bad_count() == 1u);

        mem.clear_bad("Cyberpunk2077.exe");
        assert(!mem.is_bad("Cyberpunk2077.exe"));
        assert(mem.bad_count() == 0u);

        // mark_bad for a second exe.
        mem.mark_bad("HogwartsLegacy.exe", "user_rejected");
        assert(mem.bad_count() == 1u);
        mem.clear_all_bad();
        assert(mem.bad_count() == 0u);
        std::printf("[OK] T8.6  mark_bad/is_bad/clear_bad all consistent\n");
    }

    // ── T8.7: needs_revalidation — user_locked always fresh ──────────────────
    {
        LearnedEntry e{};
        std::strncpy(e.exe, "LockedGame.exe", sizeof(e.exe) - 1);
        e.user_locked  = true;
        e.sample_count = 5u;
        // Old date — would normally be stale.
        std::strncpy(e.last_validated, "2020-01-01T00:00:00Z",
                     sizeof(e.last_validated) - 1);

        const bool stale = PerGameMemory::needs_revalidation(e, 30u);
        assert(!stale && "user_locked must never be stale");
        std::printf("[OK] T8.7  user_locked=true → needs_revalidation=false\n");
    }

    // ── T8.8: needs_revalidation — sample_count==0 → always stale ───────────
    {
        LearnedEntry e{};
        std::strncpy(e.exe, "NewGame.exe", sizeof(e.exe) - 1);
        e.user_locked  = false;
        e.sample_count = 0u;

        const bool stale = PerGameMemory::needs_revalidation(e, 30u);
        assert(stale && "sample_count==0 must be stale");
        std::printf("[OK] T8.8  sample_count=0 → needs_revalidation=true\n");
    }

    // ── T8.9: expire_stale_entries() zeroes stale data, preserves key ─────────
    {
        PerGameMemory fresh;
        fresh.generate_hardware_id();

        // Insert a fresh entry (today's date → not stale).
        // best_action MUST be set; needs_revalidation() considers an empty
        // best_action as "no useful data, needs revalidation" regardless of
        // last_validated.
        //
        // The timestamp is DERIVED from the wall clock at test time — never
        // hardcode it: needs_revalidation() measures age against TODAY, so a
        // fixed "fresh" date ages out and flips the test. That exact bomb
        // ("2026-05-14T12:00:00Z", written in May) started aborting this test
        // ~2026-06-13 and hid behind a ctest false green until 2026-07-16.
        LearnedEntry good{};
        std::strncpy(good.exe, "GoodGame.exe", sizeof(good.exe) - 1);
        std::strncpy(good.hardware_id, fresh.hardware_id(),
                     sizeof(good.hardware_id) - 1);
        good.sample_count    = 3u;
        std::strncpy(good.best_action, "PinAffinity:0xAA",
                     sizeof(good.best_action) - 1);
        good.best_core_mask  = 0xAAull;
        good.improvement_pct = 10.0f;
        good.user_locked     = false;
        {
            const std::time_t now = std::time(nullptr);
            std::tm tm_buf{};
#ifdef _WIN32
            localtime_s(&tm_buf, &now);
#else
            localtime_r(&now, &tm_buf);
#endif
            char today_iso[sizeof(good.last_validated)]{};
            std::snprintf(today_iso, sizeof(today_iso),
                          "%04d-%02d-%02dT12:00:00Z",
                          tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                          tm_buf.tm_mday);
            std::strncpy(good.last_validated, today_iso,
                         sizeof(good.last_validated) - 1);
        }
        fresh.upsert(good);

        // Insert a stale entry (old date → will be expired).
        // best_action set so the entry passes the "has data" check; only the
        // age-based predicate (>30 days) triggers expiry for this one.
        LearnedEntry old{};
        std::strncpy(old.exe, "OldGame.exe", sizeof(old.exe) - 1);
        std::strncpy(old.hardware_id, fresh.hardware_id(),
                     sizeof(old.hardware_id) - 1);
        old.sample_count    = 7u;
        std::strncpy(old.best_action, "PinAffinity:0xBB",
                     sizeof(old.best_action) - 1);
        old.best_core_mask  = 0xBBull;
        old.improvement_pct = 8.0f;
        old.user_locked     = false;
        std::strncpy(old.last_validated, "2020-01-01T00:00:00Z",
                     sizeof(old.last_validated) - 1);
        fresh.upsert(old);

        assert(fresh.count() == 2u);
        const uint32_t expired = fresh.expire_stale_entries(30u);
        assert(expired == 1u);  // only OldGame.exe

        // OldGame.exe key preserved, data zeroed.
        const LearnedEntry* e_old = fresh.find("OldGame.exe");
        assert(e_old != nullptr && "key must be preserved after expiry");
        assert(e_old->sample_count == 0u);

        // GoodGame.exe untouched.
        const LearnedEntry* e_good = fresh.find("GoodGame.exe");
        assert(e_good != nullptr);
        assert(e_good->sample_count == 3u);
        assert(e_good->best_core_mask == 0xAAull);

        std::printf("[OK] T8.9  expire_stale_entries: %u expired, key preserved\n",
                    expired);
    }

    // ── T8.10: remove() evicts entry ──────────────────────────────────────────
    {
        assert(mem.find("Cyberpunk2077.exe") != nullptr);
        mem.remove("Cyberpunk2077.exe");
        assert(mem.find("Cyberpunk2077.exe") == nullptr);
        std::printf("[OK] T8.10 remove() evicts entry\n");
    }

    std::printf("\nAll T8 tests passed.\n");
    return 0;
}
// Made with my soul - Swately <3
