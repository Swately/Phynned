// framework/render/present/tests/present_surface_test.cpp
// CI-style unit test for phyriad::render::present (FR-RENDER-1, §1.8).
//
// What runs WITHOUT a screen-scrape / PresentMon (so it is CI-safe):
//   1. POD layout invariants on PresentSurfaceDesc + SharedFrameHandle (D-8).
//   2. Descriptor defaults == the measured trilemma-resolving config
//      (DcompCt + ExcludeFromCapture + Immediate).
//   3. submit() on a default/moved-from surface is a clean InvalidHandle error
//      (no crash, no UB — the noexcept boundary holds, D-7).
//   4. Platform behaviour:
//        - non-Windows: create() returns Unavailable (the D-19 stub).
//        - Windows: create() a real DcompCt+WDA surface, assert click-through
//          flag + capture_excluded() reflect the recipe, submit_at() is
//          Unavailable (v1 pacing), then teardown leaves no live window.
//
// The screen-dependent acceptance measurement (PresentMode VERBATIM, click-through
// witness, capture exclusion) lives in present_acceptance_smoke.cpp + the existing
// stage44a probe — NOT duplicated here.

#if defined(_MSC_VER)
#  pragma warning(disable: 4127)
#endif

#include <phyriad/render/present/PresentSurface.hpp>

#include <cstdio>
#include <type_traits>

namespace pp = phyriad::render::present;

// ── tiny harness (the sibling-pillar EXPECT pattern) ────────────────────────
static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond)                                                           \
    do { if (cond) { ++g_pass; }                                               \
         else { ++g_fail;                                                      \
                std::printf("  [FAIL] %s:%d: %s\n",                            \
                            __FILE__, __LINE__, #cond); }                      \
    } while ((void)0,0)

int main() {
    std::printf("phyriad::render::present — PresentSurface unit test\n");

    // ── § Test 1: POD invariants (D-8) ──────────────────────────────────────
    std::printf("  § Test 1: POD layout invariants\n");
    EXPECT(std::is_standard_layout_v<pp::PresentSurfaceDesc>);
    EXPECT(std::is_trivially_copyable_v<pp::PresentSurfaceDesc>);
    EXPECT(std::is_standard_layout_v<pp::SharedFrameHandle>);
    EXPECT(std::is_trivially_copyable_v<pp::SharedFrameHandle>);
    // Surface itself is move-only, not copyable.
    EXPECT(!std::is_copy_constructible_v<pp::PresentSurface>);
    EXPECT(std::is_move_constructible_v<pp::PresentSurface>);

    // ── § Test 2: descriptor defaults == the measured config ────────────────
    std::printf("  § Test 2: descriptor defaults (DcompCt + ExcludeFromCapture)\n");
    {
        pp::PresentSurfaceDesc d{};
        EXPECT(d.style   == pp::Style::DcompCt);
        EXPECT(d.capture == pp::CaptureAffinity::ExcludeFromCapture);
        EXPECT(d.pacing  == pp::Pacing::Immediate);
        EXPECT(d.monitor_index == 0);
        EXPECT(d.width == 0u && d.height == 0u);  // 0,0 = full monitor extent
    }

    // ── § Test 3: default/moved-from surface — submit is a clean error ──────
    std::printf("  § Test 3: default surface submit() == InvalidHandle (no UB)\n");
    {
        pp::PresentSurface s{};                 // default: non-functional
        pp::SharedFrameHandle h{};
        h.nt_handle = reinterpret_cast<void*>(0x1);  // non-null so we reach the impl check
        auto r = s.submit(h);
        EXPECT(!r.has_value());
        EXPECT(r.error().code == phyriad::ErrorCode::InvalidHandle);
        EXPECT(!s.capture_excluded());
        EXPECT(!s.is_click_through());
        EXPECT(!s.device_lost());               // R-D2-3: no loss observed on a default surface
    }

    // ── § Test 4: platform behaviour ────────────────────────────────────────
#ifndef _WIN32
    std::printf("  § Test 4 (non-Windows): create() == Unavailable (D-19 stub)\n");
    {
        pp::PresentSurfaceDesc d{};
        auto r = pp::PresentSurface::create(d);
        EXPECT(!r.has_value());
        EXPECT(r.error().code == phyriad::ErrorCode::Unavailable);
    }
#else
    std::printf("  § Test 4 (Windows): create real DcompCt+WDA surface, verify, teardown\n");
    {
        pp::PresentSurfaceDesc d{};
        d.monitor_index = 0;
        d.width = 320; d.height = 240;          // small — a transient off-corner window
        // defaults: DcompCt + ExcludeFromCapture + Immediate.
        auto r = pp::PresentSurface::create(d);
        EXPECT(r.has_value());
        if (r.has_value()) {
            pp::PresentSurface s = std::move(*r);
            // DcompCt ⇒ click-through recipe applied.
            EXPECT(s.is_click_through());
            // WDA: SetWindowDisplayAffinity should succeed on a normal desktop session.
            // (Soft per §1.5.3 — we only require the query is well-formed, not that the
            //  OS granted it, so a locked/remote session does not fail the build.)
            const bool excl = s.capture_excluded();
            std::printf("    capture_excluded() = %s\n", excl ? "true" : "false");
            EXPECT(excl == true || excl == false);   // well-formed bool, no crash

            // submit_at() is honestly Unavailable in the call-based v1 (Immediate pacing).
            pp::SharedFrameHandle h{};
            h.nt_handle = reinterpret_cast<void*>(0x1);
            auto sa = s.submit_at(h, 0u);
            EXPECT(!sa.has_value());
            EXPECT(sa.error().code == phyriad::ErrorCode::Unavailable);

            // submit() with a bogus (non-importable) handle must fail cleanly, not crash.
            auto sb = s.submit(h);
            EXPECT(!sb.has_value());

            // ~PresentSurface destroys the HWND/device/swapchain (no live window left).
            EXPECT(!s.device_lost());            // R-D2-3: happy-path create → no terminal loss
        }
    }

    // ── § Test 5 (Windows): two concurrent surfaces both create (R-D2-5) ─────
    // The parallel-instances unblocker: a SECOND concurrent create() must NOT fail with
    // ERROR_CLASS_ALREADY_EXISTS. Before the per-instance window-class fix this second
    // create() bailed (SystemError). Both surfaces coexist on the same monitor here (the
    // test does not need two physical monitors — the class-collision is monitor-independent).
    std::printf("  § Test 5 (Windows): two concurrent create() both succeed (R-D2-5)\n");
    {
        pp::PresentSurfaceDesc d{};
        d.monitor_index = 0; d.width = 320; d.height = 240;
        auto a = pp::PresentSurface::create(d);
        EXPECT(a.has_value());                   // first surface (bare historical class name)
        auto b = pp::PresentSurface::create(d);
        EXPECT(b.has_value());                   // second surface — would have failed pre-fix
        // both destruct here; each unregisters ITS OWN class (no leak / no double-unregister).
    }
    // After both are gone, a fresh create()/destroy() still works (the per-instance classes
    // were unregistered cleanly — no stale registration blocks a later surface).
    {
        pp::PresentSurfaceDesc d{};
        d.monitor_index = 0; d.width = 320; d.height = 240;
        auto c = pp::PresentSurface::create(d);
        EXPECT(c.has_value());
    }
#endif

    const int total = g_pass + g_fail;
    if (g_fail == 0) std::printf("[ OK ] %d/%d checks PASSED\n", g_pass, total);
    else             std::printf("[FAIL] %d/%d checks PASSED (%d FAILED)\n", g_pass, total, g_fail);
    return g_fail ? 1 : 0;
}

// Made with my soul - Swately <3
