// tools/phynned-ui/PhynnedLogicNode.hpp
// Logic node for phynned-ui: connects the Phyriad UI to the agent via IPC.
//
// on_start() — attempts to connect PhynnedClient. Calculates reconnect interval.
// tick()     — if connected: reads SHM → compresses to PhynnedSnapshotMini → publishes.
//              if not: retries reconnect every ~1s (rate-limited by TSC).
//
// Multi-inlet: InputEvent (idx 0) + WindowState (idx 1).
// Single outlet: PhynnedAppState (idx 0).
//
// Large arrays (targets[], metrics[]) are exposed to panels via the
// client() accessor — NOT passed through the Ring (avoids copying 6+ KB/frame).
//
// Threading: single-thread (logic in GraphRuntime).
// Privilege: none — PhynnedClient only opens an existing SHM mapping.
#pragma once
#include "PhynnedAppState.hpp"
#include "PhynnedTrayIcon.hpp"
#include "UserSettings.hpp"
#include "widgets/settings_panel.hpp"  // current_settings()

#include <phyriad/ui/types/InputEvent.hpp>
#include <phyriad/ui/types/WindowState.hpp>
#include <phyriad/node/Port.hpp>
#include <phyriad/hal/Timestamp.hpp>
#include <phyriad/schema/Error.hpp>
#include <phynned/ipc/PhynnedClient.hpp>
#include <phynned/version.hpp>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <expected>

// ─────────────────────────────────────────────────────────────────────────────
// PhynnedLogicNode
// ─────────────────────────────────────────────────────────────────────────────
class PhynnedLogicNode {
public:
    using output_type = PhynnedAppState;

    // ── Ports ────────────────────────────────────────────────────────────────
    phyriad::node::Inlet<phyriad::ui::InputEvent>  in_input{};
    phyriad::node::Inlet<phyriad::ui::WindowState> in_window{};
    phyriad::node::Outlet<PhynnedAppState>       out_state{};

    // ── Multi-port routing ───────────────────────────────────────────────────
    [[nodiscard]] void* inlet_at_erased(std::size_t i) noexcept {
        if (i == 0u) return &in_input;
        if (i == 1u) return &in_window;
        return nullptr;
    }

    [[nodiscard]] void* outlet_at_erased(std::size_t i) noexcept {
        if (i == 0u) return &out_state;
        return nullptr;
    }

    [[nodiscard]] phyriad::node::Outlet<PhynnedAppState>& outlet() noexcept {
        return out_state;
    }

    // ── Client access (for draw_widgets / panels) ────────────────────────────
    [[nodiscard]] phynned::ipc::PhynnedClient& client() noexcept { return client_; }
    [[nodiscard]] const phynned::ipc::PhynnedClient& client() const noexcept { return client_; }

    // ── Lifecycle ────────────────────────────────────────────────────────────
    [[nodiscard]] std::expected<void, phyriad::Error> on_start() noexcept {
        // Compute TSC-based reconnect interval (~1 second).
        // calibrate_tsc_freq() sleeps ~10 ms once, then caches the result.
        const uint64_t tsc_freq = phyriad::hal::calibrate_tsc_freq();
        reconnect_interval_tsc_ = (tsc_freq > 0u) ? tsc_freq : 2'000'000'000ull;
        last_reconnect_tsc_ = 0u;

        // Copy version string into state. Built from PHYNNED_VERSION_STRING so
        // the displayed version always matches the released tag (release-please
        // keeps phynned/version.hpp in sync on every release).
        const int written = std::snprintf(state_.agent_version,
                                          sizeof(state_.agent_version),
                                          "Phynned %s", PHYNNED_VERSION_STRING);
        if (written < 0 || written >= static_cast<int>(sizeof(state_.agent_version))) {
            state_.agent_version[sizeof(state_.agent_version) - 1] = '\0';
        }
        state_.op_mode = 0u;  // Auto by default

        // ── System tray (§9.5) ───────────────────────────────────────────────
        // Create a message-only HWND so we can receive tray callbacks without
        // needing the main window HWND (which Phyriad manages internally).
#ifdef _WIN32
        {
            // We register a minimal WNDCLASS and create a hidden message window.
            // The class is registered once per process.
            WNDCLASSEXW wc{};
            wc.cbSize        = sizeof(wc);
            wc.lpfnWndProc   = tray_wndproc_;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.lpszClassName = kTrayClassName_;
            RegisterClassExW(&wc);  // ok to fail if already registered

            tray_msg_hwnd_ = CreateWindowExW(
                0, kTrayClassName_, nullptr, 0,
                0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);

            if (tray_msg_hwnd_) {
                tray_.create(tray_msg_hwnd_);
                tray_.set_state(TrayState::Gray);
            }
        }
#endif

        // Attempt initial connection (non-blocking, failure is OK).
        (void)client_.connect();
        return {};
    }

    [[nodiscard]] std::expected<void, phyriad::Error> on_stop() noexcept {
        tray_.destroy();
#ifdef _WIN32
        if (tray_msg_hwnd_) {
            DestroyWindow(tray_msg_hwnd_);
            tray_msg_hwnd_ = nullptr;
        }
#endif
        client_.disconnect();
        return {};
    }

    // ── tick — one logic frame ───────────────────────────────────────────────
    [[nodiscard]] std::expected<void, phyriad::Error> tick() noexcept {
        // 1. Poll window state for framebuffer dimensions.
        //    phyriad::ui::WindowState exposes fb_width/fb_height (framebuffer pixel size);
        //    PhynnedAppState stores these as win_width/win_height (same value sin HiDPI;
        //    en HiDPI representan los pixeles renderizados, que es lo que ImGui usa).
        if (in_window.connected()) {
            auto ws_r = in_window.receive();
            if (ws_r.has_value()) {
                state_.win_width  = static_cast<uint32_t>(ws_r->fb_width);
                state_.win_height = static_cast<uint32_t>(ws_r->fb_height);
            }
        }

        // 2. Drain input events (we don't act on them, just consume).
        if (in_input.connected()) {
            (void)in_input.receive();
        }

        // 3. Try reconnect if disconnected (rate-limited).
        if (!client_.is_connected()) {
            const uint64_t now = phyriad::hal::rdtsc();
            if ((now - last_reconnect_tsc_) >= reconnect_interval_tsc_) {
                last_reconnect_tsc_ = now;
                (void)client_.connect();
            }
            state_.snap.agent_connected = 0u;
            (void)out_state.publish(state_);
            return {};
        }

        // 4. Sync SHM → PhynnedSnapshotMini.
        // DEAD-AGENT DETECTION (2026-07-19 soak fix): the mapping outlives a
        // hard-killed agent, and its agent_connected byte stays 1 forever —
        // the soak UI kept rendering frozen stale data as "connected". The
        // real liveness signal is last_publish_tsc advancing (~1 Hz); if it
        // hasn't moved for ~5 s of frames, report disconnected so every panel
        // shows its honest "agent is not running" state.
        {
            const uint64_t pub = client_.state()
                ? client_.state()->last_publish_tsc : 0ull;
            if (pub != last_seen_publish_tsc_) {
                last_seen_publish_tsc_ = pub;
                frames_publish_stalled_ = 0u;
            } else if (frames_publish_stalled_ < 0xFFFFu) {
                ++frames_publish_stalled_;
            }
        }
        if (frames_publish_stalled_ > kPublishStallFrames) {
            state_.snap.agent_connected = 0u;
            (void)out_state.publish(state_);
            return {};
        }
        state_.snap.agent_connected = 1u;

        const auto targets   = client_.targets();
        const auto metrics   = client_.metrics();
        const auto decisions = client_.decisions();

        state_.snap.target_count   = static_cast<uint32_t>(targets.size());
        state_.snap.decision_count = static_cast<uint32_t>(decisions.size());

        // Read extended state from PhynnedStateHeader.
        if (const auto* st = client_.state()) {
            state_.snap.action_count    = st->n_active_actions;
            state_.snap.privilege_level = st->privilege_level;
            state_.snap.bench_phase     = st->bench_phase;
            state_.snap.bad_count           = st->bad_count;
            state_.snap.deep_idle           = st->deep_idle;
            state_.snap.watchdog_ok         = st->watchdog_ok;
            // CCD Load Defense telemetry
            state_.snap.ccd_defense_count   = st->ccd_defense_count;
            state_.snap.ccd_defense_cpu_pct = st->ccd_defense_cpu_pct;
            // MASS-router: full internal tracked count (≥ target_count shown).
            state_.snap.total_tracked       = st->n_tracked_total;
            state_.etw_active               = st->etw_active;
            // Hardware classification (v1.0): static after agent probe, but
            // we copy each tick anyway (idempotent; ~4 bytes, no perf cost).
            state_.snap.cpu_class    = st->cpu_class;
            state_.snap.ccd_count    = st->ccd_count;
            state_.snap.p_core_count = st->p_core_count;
            state_.snap.e_core_count = st->e_core_count;
            // Runtime control state (Start/Pause/Reset flow).
            state_.snap.policies_paused = st->policies_paused;
            // MR-2 background corral mode (DRY-RUN / LIVE + E5 coexistence block).
            state_.snap.corral_live          = st->corral_live;
            state_.snap.corral_coexist_block = st->corral_coexist_block;
            // W4 global profile (Monitor / Games / GamesCorral / Full).
            state_.snap.profile              = st->profile;
            // Agent self-resource accounting (published by SelfMonitor
            // every ~500 ms). Routes into PhynnedAppState's top-level
            // fields, not inside `snap`, because the existing widgets
            // (Dashboard + Advanced) already read from there.
            state_.self_cpu_pct = st->self_cpu_pct;
            state_.self_rss_mb  = st->self_rss_mb;
        }

        // Aggregate pressure + migration sum across all targets.
        float pressure_sum = 0.f;
        uint32_t mig_total = 0u;
        const uint32_t n_top = static_cast<uint32_t>(
            std::min<std::size_t>(targets.size(), 5u));

        for (uint32_t i = 0u; i < n_top; ++i) {
            state_.snap.top_target_pids[i] = targets[i].pid;
            std::strncpy(state_.snap.top_target_names[i],
                         targets[i].name, 31);
            state_.snap.top_target_names[i][31] = '\0';
        }
        // Zero remainder.
        for (uint32_t i = n_top; i < 5u; ++i) {
            state_.snap.top_target_pids[i] = 0u;
            state_.snap.top_target_names[i][0] = '\0';
        }

        for (std::size_t i = 0u; i < metrics.size(); ++i) {
            pressure_sum += static_cast<float>(metrics[i].pressure_level);
            mig_total    += metrics[i].migrations_per_sec;
        }
        state_.snap.aggregate_pressure   = (n_top > 0u)
            ? pressure_sum / static_cast<float>(targets.size()) : 0.f;
        state_.snap.total_migrations_obs = mig_total;
        state_.snap.last_sync_tsc        = phyriad::hal::rdtsc();

        // 5. Update system tray state (§9.5).
        update_tray(state_);

        // 6. Pump tray message window (non-blocking).
#ifdef _WIN32
        if (tray_msg_hwnd_) {
            MSG msg{};
            while (PeekMessageW(&msg, tray_msg_hwnd_, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
#endif

        // 7. Publish.
        (void)out_state.publish(state_);
        return {};
    }

private:
    phynned::ipc::PhynnedClient client_{};
    PhynnedAppState            state_{};
    uint64_t                 last_reconnect_tsc_{0u};
    uint64_t                 reconnect_interval_tsc_{2'000'000'000ull};

    // Dead-agent detection (2026-07-19): last_publish_tsc must keep advancing
    // (~1 Hz) for the agent to count as alive. ~300 frames at the 60 Hz paced
    // loop ≈ 5 s of publish silence → report disconnected.
    uint64_t last_seen_publish_tsc_{0ull};
    uint16_t frames_publish_stalled_{0u};
    static constexpr uint16_t kPublishStallFrames = 300u;

    // ── System tray (§9.5) ───────────────────────────────────────────────────
    PhynnedTrayIcon tray_{};
    TrayState     last_tray_state_{TrayState::Gray};
    uint32_t      last_target_count_{0u};  // for "new target detected" toast

#ifdef _WIN32
    HWND tray_msg_hwnd_{nullptr};
    static constexpr const wchar_t* kTrayClassName_ = L"PhynnedTrayMsgWnd";

    /// Minimal WndProc for the message-only tray window.
    static LRESULT CALLBACK tray_wndproc_(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) noexcept
    {
        // Retrieve 'this' stored in window user data.
        PhynnedLogicNode* self = nullptr;
        if (uMsg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<PhynnedLogicNode*>(cs->lpCreateParams);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<PhynnedLogicNode*>(
                GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        }

        if (self) {
            if (uMsg == PhynnedTrayIcon::kTrayMsg) {
                self->tray_.on_tray_message(wParam, lParam);
                return 0;
            }
            if (uMsg == WM_COMMAND) {
                self->tray_.on_menu_command(LOWORD(wParam));
                return 0;
            }
        }
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
#endif

    // ── Tray state machine ────────────────────────────────────────────────────
    void update_tray(const PhynnedAppState& s) noexcept
    {
        if (!tray_.is_active()) return;

        // Determine new tray state.
        TrayState new_state = TrayState::Gray;
        if (s.snap.agent_connected) {
            if (s.snap.watchdog_ok == 0u) {
                new_state = TrayState::Red;     // watchdog stall → likely stuck
            } else if (s.snap.privilege_level < 2u) {
                new_state = TrayState::Orange;  // no admin — degraded
            } else if (s.op_mode == 1u) {
                new_state = TrayState::Yellow;  // Assist mode — awaiting confirm
            } else {
                new_state = TrayState::Green;   // healthy Auto / Manual
            }
        }

        // Update icon and fire toasts on state transitions. All toasts
        // are gated by UserSettings — users can silence individual
        // categories from the Settings tab. Toast preferences are read
        // each call (cheap; static load + flag).
        const phynned_ui::UserSettings& prefs = phynned_ui::current_settings();

        if (new_state != last_tray_state_) {
            const TrayState old_state = last_tray_state_;
            last_tray_state_ = new_state;
            tray_.set_state(new_state);

#ifdef _WIN32
            // Toast: regression detected (high signal — only fires when
            // AutoRevertGuard auto-reverted a policy).
            if (new_state == TrayState::Red && s.snap.agent_connected &&
                prefs.toast_regression)
            {
                tray_.show_toast(
                    L"Phynned - Regression Detected",
                    L"A policy was reverted automatically (performance got worse).");
            }
            // Toast: first successful connection from disconnected state.
            if ((new_state == TrayState::Green || new_state == TrayState::Orange) &&
                old_state == TrayState::Gray &&
                prefs.toast_agent_connected)
            {
                tray_.show_toast(L"Phynned", L"Agent connected and monitoring.");
            }
#else
            (void)old_state;
#endif
        }

#ifdef _WIN32
        // Toast: NEW GAME detected (filtered to TargetKind::Game only —
        // background apps, browsers and launcher helpers no longer pop a
        // notification each time they enter the target_buf). Tracks the
        // set of "PIDs we've already toasted for" so re-classification or
        // reorder of top_target_pids doesn't re-fire the same toast.
        if (s.snap.agent_connected && prefs.toast_game_detected) {
            for (const auto& t : client_.targets()) {
                if (t.kind != phynned::observer::TargetKind::Game) continue;
                if (was_pid_toasted_(t.pid)) continue;
                remember_toasted_pid_(t.pid);
                wchar_t title[64]{};
                MultiByteToWideChar(CP_ACP, 0, t.name, -1, title, 63);
                tray_.show_toast(title,
                    L"Game detected - Phynned is optimizing it.");
                break;  // at most one toast per tick
            }
        }
#endif
        last_target_count_ = s.snap.target_count;
    }

    // ── Toasted-PID set (avoid re-toasting the same game) ────────────────
    // Tiny ring buffer of recent PIDs that already triggered a "Game
    // detected" toast. PIDs are reaped from the ring when a process
    // exits (PID disappears from observer.snapshot for several ticks).
    static constexpr uint32_t kToastedRing = 16u;
    uint32_t toasted_pids_[kToastedRing]{};
    uint32_t toasted_n_{0u};

    [[nodiscard]] bool was_pid_toasted_(uint32_t pid) const noexcept {
        for (uint32_t i = 0u; i < toasted_n_; ++i) {
            if (toasted_pids_[i] == pid) return true;
        }
        return false;
    }
    void remember_toasted_pid_(uint32_t pid) noexcept {
        if (toasted_n_ < kToastedRing) {
            toasted_pids_[toasted_n_++] = pid;
        } else {
            // Ring full — evict the oldest.
            for (uint32_t i = 1u; i < kToastedRing; ++i)
                toasted_pids_[i - 1u] = toasted_pids_[i];
            toasted_pids_[kToastedRing - 1u] = pid;
        }
    }
};
// Made with my soul - Swately <3
