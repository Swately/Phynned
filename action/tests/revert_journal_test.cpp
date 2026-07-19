// action/tests/revert_journal_test.cpp
// Unit test for RevertJournal (DR1). Proves, on the box:
//   1. record 3 PENDING placements (durably), mark 2 APPLIED.
//   2. WRITE-AHEAD durability: a PENDING record is on disk (readable by a fresh
//      journal instance) BEFORE mark_applied is ever called.
//   3. Simulated crash (no mark_reverted) → a fresh recover() returns the 2
//      APPLIED + 1 PENDING with the CORRECT prev_masks.
//   4. pid-recycle guard: process_still_matches() drops a record whose
//      creation-time no longer matches a live process, and accepts one that does.
//
// Self-contained: only depends on phynned::action::RevertJournal + <windows.h>.
//
#include <phynned/action/RevertJournal.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

using phynned::action::RevertJournal;
using phynned::action::RevertKey;
using phynned::action::RevertRecord;
using phynned::action::RevertStatus;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Find one recovered record by pid (nullptr if absent).
static const RevertRecord* by_pid(const std::vector<RevertRecord>& v, uint32_t pid) {
    for (const auto& r : v) if (r.pid == pid) return &r;
    return nullptr;
}

int main() {
    std::printf("revert_journal_test — DR1 write-ahead journal\n");

    // Fresh temp journal (never touch the operator's real journal).
    char tmp[MAX_PATH]{};
    GetTempPathA(sizeof(tmp), tmp);
    std::string jpath = std::string(tmp) + "phynned_revert_journal_test.bin";
    DeleteFileA(jpath.c_str());

    // Three synthetic placements. prev_masks are distinct so we can verify each
    // survives round-trip exactly.
    const uint32_t P1 = 1001u, P2 = 1002u, P3 = 1003u;
    const uint64_t C1 = 0x11111111ull, C2 = 0x22222222ull, C3 = 0x33333333ull;
    const uint64_t PREV1 = 0x00000000000000FFull; // cores 0-7
    const uint64_t PREV2 = 0x000000000000FF00ull; // cores 8-15
    const uint64_t PREV3 = 0xFFFFFFFFFFFFFFFFull; // all cores
    const uint64_t NEWM  = 0x000000000000000Full; // placed onto cores 0-3

    // ── Phase 1: record 3 PENDING ─────────────────────────────────────────────
    {
        RevertJournal j;
        check(j.open(jpath.c_str()), "open journal");

        j.record_pending(P1, "alpha.exe", C1, PREV1, NEWM);

        // ── Phase 2 proof: WRITE-AHEAD durability. Before ANY mark_applied, a
        //    SEPARATE journal instance must already see P1 as PENDING on disk.
        {
            RevertJournal probe;
            check(probe.open(jpath.c_str()), "open second (probe) instance");
            auto pre = probe.recover();
            const RevertRecord* r = by_pid(pre, P1);
            check(r != nullptr, "write-ahead: PENDING P1 durable on disk pre-mark");
            check(r != nullptr && r->status == RevertStatus::Pending,
                  "write-ahead: on-disk status is PENDING");
            check(r != nullptr && r->prev_mask == PREV1,
                  "write-ahead: on-disk prev_mask correct");
        }

        j.record_pending(P2, "beta.exe",  C2, PREV2, NEWM);
        j.record_pending(P3, "gamma.exe", C3, PREV3, NEWM);

        // Mark 2 of 3 APPLIED (P1, P2). P3 stays PENDING (crash-in-window case).
        j.mark_applied(RevertKey{P1, C1});
        j.mark_applied(RevertKey{P2, C2});

        // Phase 2b: simulate crash — do NOT mark_reverted anything, just drop the
        // journal object without a clean shutdown revert.
    }

    // ── Phase 3: fresh recover() sees 2 APPLIED + 1 PENDING, correct prev_masks
    {
        RevertJournal j;
        check(j.open(jpath.c_str()), "reopen journal after crash");
        auto recs = j.recover();
        check(recs.size() == 3u, "recover returns all 3 surviving records");

        const RevertRecord* r1 = by_pid(recs, P1);
        const RevertRecord* r2 = by_pid(recs, P2);
        const RevertRecord* r3 = by_pid(recs, P3);
        check(r1 && r1->status == RevertStatus::Applied,  "P1 recovered APPLIED");
        check(r2 && r2->status == RevertStatus::Applied,  "P2 recovered APPLIED");
        check(r3 && r3->status == RevertStatus::Pending,  "P3 recovered PENDING");
        check(r1 && r1->prev_mask == PREV1, "P1 prev_mask intact (0x00..FF)");
        check(r2 && r2->prev_mask == PREV2, "P2 prev_mask intact (0xFF00)");
        check(r3 && r3->prev_mask == PREV3, "P3 prev_mask intact (all cores)");
        check(r1 && std::strcmp(r1->exe_name, "alpha.exe") == 0, "P1 exe intact");

        // Idempotency check: marking one reverted removes it from recover().
        j.mark_reverted(RevertKey{P1, C1});
        auto recs2 = j.recover();
        check(recs2.size() == 2u, "after mark_reverted P1, recover returns 2");
        check(by_pid(recs2, P1) == nullptr, "reverted P1 no longer recovered");
    }

    // ── Phase 4: pid-recycle guard ─────────────────────────────────────────────
    {
        const uint32_t self = GetCurrentProcessId();
        const uint64_t real_ct = RevertJournal::query_creation_time(self);
        check(real_ct != 0u, "read own creation-time");

        // (a) Record whose creation-time MATCHES the live process -> accepted.
        RevertRecord good{};
        good.pid = self;
        good.creation_time = real_ct;
        good.prev_mask = 0xFull;
        good.exe_name[0] = '\0'; // skip exe check; test the creation-time gate
        check(RevertJournal::process_still_matches(good),
              "guard ACCEPTS record whose creation-time matches");

        // (b) Same pid, WRONG creation-time (the pid-recycle case) -> dropped.
        RevertRecord recycled = good;
        recycled.creation_time = real_ct ^ 0xDEADBEEFull;
        check(!RevertJournal::process_still_matches(recycled),
              "guard DROPS record whose creation-time no longer matches");

        // (c) A pid that is almost certainly dead -> dropped.
        RevertRecord dead{};
        dead.pid = 0x7FFFFFF0u; // implausible live pid
        dead.creation_time = real_ct;
        check(!RevertJournal::process_still_matches(dead),
              "guard DROPS record for a non-live pid");
    }

    DeleteFileA(jpath.c_str());
    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
// Made with my soul - Swately <3
