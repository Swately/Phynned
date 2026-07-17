// ipc/include/phynned/ipc/PhynnedClient.hpp
// PhynnedClient — read-only view into the agent's shared memory.
//
// Used by standard_window and other UI clients to display agent data.
// Connect to the named shared memory created by phynned-agent.
// All data is read-only; sending commands to the agent uses a separate
// command channel (not yet implemented — Phase 2).
//
// Threading: connect()/disconnect() from UI init. All other methods from
//            the UI/render thread. NOT thread-safe internally.
// Resource:  1 MB view of shared memory; no heap allocation after connect().
// Privilege: OpenFileMapping — no admin required.
//
#pragma once

#include <phynned/ipc/PhynnedProtocol.hpp>
#include <phyriad/schema/Error.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <phyriad/hal/MemoryOrder.hpp>

namespace phynned::ipc {

class PhynnedClient {
public:
    PhynnedClient() noexcept = default;
    ~PhynnedClient() noexcept { disconnect(); }

    PhynnedClient(PhynnedClient const&)            = delete;
    PhynnedClient& operator=(PhynnedClient const&) = delete;

    /// Open the shared memory created by phynned-agent.
    /// `shm_name` must match AgentConfig::shm_name (default "Local\\PhynnedAgent.v1").
    [[nodiscard]] std::expected<void, phyriad::Error>
    connect(const char* shm_name = "Local\\PhynnedAgent.v1") noexcept;

    /// Close the shared memory view. Safe to call if not connected.
    void disconnect() noexcept;

    [[nodiscard]] bool is_connected() const noexcept { return layout_ != nullptr; }

    // ── Read-only views — valid while connected() ─────────────────────────
    [[nodiscard]] std::span<const observer::TargetProcess> targets() const noexcept {
        if (!layout_) return {};
        const uint32_t n = layout_->state.n_targets;
        return {layout_->targets, n};
    }

    [[nodiscard]] std::span<const observer::TargetMetrics> metrics() const noexcept {
        if (!layout_) return {};
        const uint32_t n = layout_->state.n_targets;
        return {layout_->metrics, n};
    }

    [[nodiscard]] std::span<const policy::PolicyDecision> decisions() const noexcept {
        if (!layout_) return {};
        const uint32_t n = layout_->state.n_decisions;
        return {layout_->decisions, n};
    }

    [[nodiscard]] const PhynnedStateHeader* state() const noexcept {
        return layout_ ? &layout_->state : nullptr;
    }

    /// Drain new action log entries since the last call to drain_actions().
    /// Uses the external-cursor multi-reader API (write_cursor / peek_at) —
    /// independent from any other reader and from the ring's own read_seq_.
    /// Returns count written to `out`.
    uint32_t drain_actions(action::ActionLogEntry* out, uint32_t max) noexcept {
        if (!layout_ || !out || max == 0u) return 0u;
        const auto& ring = layout_->action_log;
        const uint64_t head = ring.write_cursor();
        uint32_t n = 0u;
        while (action_cursor_ < head && n < max) {
            // Skip-ahead guard: if the producer has wrapped past us, jump to
            // the oldest still-live slot.
            if (head - action_cursor_ > action::kActionLogCap) {
                action_cursor_ = head - action::kActionLogCap;
            }
            action::ActionLogEntry e{};
            if (ring.peek_at(action_cursor_, e) && e.target_pid != 0u) {
                out[n++] = e;
            }
            ++action_cursor_;
        }
        return n;
    }

    /// Read-only view of the entire action log ring buffer (128 slots).
    /// Entries with target_pid == 0 are empty / not yet used.
    /// The caller does NOT consume entries; use this for display-only panels.
    [[nodiscard]] std::span<const action::ActionLogEntry> action_log() const noexcept {
        if (!layout_) return {};
        return layout_->action_log.slots_view();
    }

    // ── Commands (UI → agent) ─────────────────────────
    /// True if this connection has write access to the command slot.
    [[nodiscard]] bool can_send_commands() const noexcept {
        return layout_ != nullptr && !ro_only_;
    }

    /// Submit a command to the agent. Returns the seq number assigned (>0 on
    /// success) or 0 if the connection is read-only / not connected.
    /// Async: the agent processes the command on its next tick. Use
    /// `command_ack()` to poll for completion (compare with returned seq).
    uint64_t send_command(uint32_t cmd_kind,
                          uint32_t target_pid = 0u,
                          uint64_t arg1 = 0ull,
                          uint64_t arg2 = 0ull) noexcept {
        if (!can_send_commands()) return 0ull;
        auto& slot = layout_->command_slot;
        // Write fields first, then bump seq with release so agent acquires
        // a consistent snapshot.
        slot.cmd_kind   = cmd_kind;
        slot.target_pid = target_pid;
        slot.arg1       = arg1;
        slot.arg2       = arg2;
        return slot.seq.fetch_add(1ull, std::memory_order_release) + 1ull;  // HAL: release ordering — CAS success or paired ack
    }

    /// Latest ack from agent (monotonic — matches seq once command is done).
    [[nodiscard]] uint64_t command_ack() const noexcept {
        if (!layout_) return 0ull;
        return phyriad::hal::seq_load_acquire(layout_->command_slot.ack);
    }

private:
    PhynnedShmLayout* layout_        {nullptr};
    uint64_t        action_cursor_ {0ull};
    bool            ro_only_       {false};  // read-only fallback

#ifdef _WIN32
    void* map_handle_{nullptr};
    void* file_handle_{nullptr};
#else
    int shm_fd_{-1};
#endif
};

} // namespace phynned::ipc
// Made with my soul - Swately <3
