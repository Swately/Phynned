// framework/ui/tests/ui_compile_test.cpp
// Compile-time header validation for the phyriad::ui pillar.
//
// Verifies that all ui types compile cleanly under any supported compiler.
// No runtime assertions needed — successful compilation IS the test.
//
#include <phyriad/ui/types/InputEvent.hpp>
#include <phyriad/ui/types/WindowState.hpp>
#include <phyriad/ui/ApplicationConfig.hpp>

// ── §1 — POD layout ──────────────────────────────────────────────────────────
static_assert(sizeof(phyriad::ui::InputEvent) == 64u,
    "InputEvent must be 64 bytes");
static_assert(alignof(phyriad::ui::InputEvent) == 8u,
    "InputEvent must be 8-byte aligned");

static_assert(sizeof(phyriad::ui::WindowState) == 32u,
    "WindowState must be 32 bytes");
static_assert(alignof(phyriad::ui::WindowState) == 8u,
    "WindowState must be 8-byte aligned");

// ── §2 — ProfileKind enum ─────────────────────────────────────────────────────
static_assert(static_cast<uint8_t>(phyriad::ProfileKind::LATENCY)  == 0u);
static_assert(static_cast<uint8_t>(phyriad::ProfileKind::BALANCED) == 1u);
static_assert(static_cast<uint8_t>(phyriad::ProfileKind::POWER)    == 2u);

// ── §3 — ApplicationConfig defaults ──────────────────────────────────────────
static_assert([] {
    phyriad::ui::ApplicationConfig cfg{};
    return cfg.window.width  == 1280u
        && cfg.window.height == 720u
        && cfg.frame_arena_capacity == (4u * 1024u * 1024u);
}());

// ── §4 — InputEvent::Kind coverage ────────────────────────────────────────────
static_assert(static_cast<uint8_t>(phyriad::ui::InputEvent::Kind::MouseMove)    == 0u);
static_assert(static_cast<uint8_t>(phyriad::ui::InputEvent::Kind::GamepadButton) == 12u);

// ── §5 — WindowState flag bitmasks are non-overlapping ────────────────────────
static_assert((phyriad::ui::WindowState::kFocused
             | phyriad::ui::WindowState::kMinimized
             | phyriad::ui::WindowState::kFullscreen
             | phyriad::ui::WindowState::kHovered) == 0x0Fu);

int main() { return 0; }
// Made with my soul - Swately <3
