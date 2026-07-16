// framework/graph/include/phyriad/api/Validation.hpp
// DSL-level error type for graph construction failures.
//
// ConfigError is a fixed-size, noexcept-safe struct describing why graph build
// failed. The `message` field (120 chars, null-terminated) holds a human-readable
// explanation. All factory helpers are noexcept.
//
// ConfigErrorCode:
//   OK               — no error
//   DuplicateNodeName — two nodes registered with the same name
//   UnknownNodeName  — a wire references a name not in the node registry
//   TypeMismatch     — output_hash of from-node ≠ input_hash of to-node
//   EmptyGraph       — build() called with 0 nodes or 0 wires
//   CycleDetected    — GraphValidator found a directed cycle
//   UnreachableNode  — GraphValidator found a node unreachable from any source
//   ValidationFailed — GraphValidator returned a non-OK result (code in message)
//
// §3.H of PHASE_H_IMPLEMENTATION_PATTERNS.md

#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>

namespace phyriad::api {

// ── ConfigErrorCode ───────────────────────────────────────────────────────────
enum class ConfigErrorCode : uint8_t {
    OK                = 0,
    DuplicateNodeName = 1,
    UnknownNodeName   = 2,
    TypeMismatch      = 3,
    EmptyGraph        = 4,
    CycleDetected     = 5,
    UnreachableNode   = 6,
    ValidationFailed  = 7,
};

// ── ConfigError ───────────────────────────────────────────────────────────────
// Fixed-size (140 bytes). Suitable for stack allocation, std::expected, NRVO.
struct ConfigError {
    ConfigErrorCode  code{ConfigErrorCode::OK};
    uint8_t          _pad[3]{};
    uint32_t         offending_node_id{UINT32_MAX};  // set for node-specific errors
    uint32_t         offending_wire_id{UINT32_MAX};  // set for wire-specific errors
    uint32_t         _pad2{};
    char             message[120]{};  // null-terminated, human-readable

    [[nodiscard]] bool ok() const noexcept {
        return code == ConfigErrorCode::OK;
    }

    [[nodiscard]] std::string_view message_view() const noexcept {
        return std::string_view{message, std::strlen(message)};
    }

    [[nodiscard]] static ConfigError make_ok() noexcept { return {}; }

    [[nodiscard]] static ConfigError make(
        ConfigErrorCode     c,
        std::string_view    msg,
        uint32_t            node_id = UINT32_MAX,
        uint32_t            wire_id = UINT32_MAX) noexcept
    {
        ConfigError e{};
        e.code              = c;
        e.offending_node_id = node_id;
        e.offending_wire_id = wire_id;
        const auto len      = std::min(msg.size(), sizeof(e.message) - 1u);
        std::memcpy(e.message, msg.data(), len);
        e.message[len]      = '\0';
        return e;
    }
};

} // namespace phyriad::api
// Made with my soul - Swately <3
