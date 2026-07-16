// framework/ui/include/phyriad/ui/types/InputEvent.hpp
// phyriad::ui input event message — 64 bytes, alignof=8.
//
// Carries all windowing/input events from UIThreadNode to AppLogicNode.
// One message per raw event; packs device state, coordinates, and raw payload.
//
#pragma once
#include <phyriad/schema/PodMessage.hpp>
#include <cstdint>

namespace phyriad::ui {

struct InputEvent {
    enum class Kind : uint8_t {
        MouseMove      = 0,
        MouseButton    = 1,
        MouseEnter     = 2,
        MouseLeave     = 3,
        Scroll         = 4,
        Key            = 5,
        Char           = 6,
        WindowResize   = 7,
        WindowClose    = 8,
        WindowFocus    = 9,
        FileDrop       = 10,
        GamepadAxis    = 11,
        GamepadButton  = 12,
    };

    Kind     kind;           // offset 0
    uint8_t  action;         // offset 1
    uint8_t  button;         // offset 2
    uint8_t  _pad0;          // offset 3
    uint32_t modifiers;      // offset 4
    uint64_t timestamp_tsc;  // offset 8
    int32_t  x;              // offset 16
    int32_t  y;              // offset 20
    uint32_t key_or_char;    // offset 24
    uint32_t scan_code;      // offset 28
    uint8_t  payload[32];    // offset 32
    // total = 64 bytes
};

static_assert(sizeof(InputEvent)  == 64u);
static_assert(alignof(InputEvent) == 8u);
static_assert(schema::PodMessage<InputEvent>);

} // namespace phyriad::ui
// Made with my soul - Swately <3
