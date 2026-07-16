// apps/ayama/tools/ayama-ui/main.cpp
// ayama-ui - standalone Ayama optimizer window.
//
// Follows the standard_window pattern (Phyriad+ImGui reference):
//   - phyriad::ui::Application::run(cfg, build_lambda)
//   - AyamaLogicNode - logic: reads agent SHM, publishes AyamaAppState
//   - RenderNode<AyamaAppState> - render: draw_widgets() with tabs
//   - Wire: "ui"→"logic"→"render"
//
// AyamaClient (IPC with the agent) lives in AyamaLogicNode.
// Panels access it via g_logic (file-static) to read large arrays
// (targets[], metrics[]) without copying them through the Ring.

// ── Hybrid-GPU laptop hint: prefer the discrete GPU ───────────────────────────
// On laptops with both an integrated (Intel iGPU) and a discrete GPU (NVIDIA
// / AMD), Windows by default runs apps on the iGPU to save power. This breaks
// Ayama on machines where the iGPU driver is missing or stuck on the
// Microsoft Basic Display Adapter (OpenGL 1.1 only) — glfwCreateWindow()
// fails because we request a 3.3 Core context the iGPU can't provide.
//
// Exporting these two symbols from the .exe is the documented escape hatch:
//   - NvOptimusEnablement (NVIDIA Optimus, since 2013): when set to 1, the
//     NVIDIA driver runs this executable on the discrete GPU.
//   - AmdPowerXpressRequestHighPerformance (AMD PowerXpress / Enduro): same
//     mechanism for AMD hybrid graphics.
// Both are read by the GPU driver at process load time. Cost: zero on
// systems without hybrid graphics (the driver simply ignores them).
#ifdef _WIN32
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement                = 1;
    __declspec(dllexport) int           AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#include "AyamaAppState.hpp"
#include "AyamaLogicNode.hpp"
#include "AgentLauncher.hpp"

#include "widgets/cpu_class_helpers.hpp"
#include "widgets/dashboard_panel.hpp"
#include "widgets/setup_panel.hpp"
#include "widgets/targets_panel.hpp"
#include "widgets/policies_panel.hpp"
#include "widgets/actions_panel.hpp"
#include "widgets/bench_panel.hpp"
#include "widgets/advanced_panel.hpp"
#include "widgets/anticheat_warning.hpp"
#include "widgets/settings_panel.hpp"

#include <phyriad/hal/Timestamp.hpp>     // calibrate_tsc_freq / rdtsc
#include <cstdlib>      // std::exit

#include <phyriad/ui/Application.hpp>
#include <phyriad/ui/ApplicationConfig.hpp>
#include <phyriad/ui/RenderNode.hpp>
#include <phyriad/api/NodeRegistry.hpp>
#include <phyriad/api/NodeHandle.hpp>
#include <phyriad/api/GraphDSL.hpp>
#include <phyriad/api/placement.hpp>
#include <phyriad/schema/SchemaHash.hpp>
#include <phyriad/schema/Error.hpp>
#include <imgui.h>
#include <imgui_internal.h>

// ─────────────────────────────────────────────────────────────────────────────
// File-static logic node pointer.
// Set before any tick() fires; used by draw_widgets to access AyamaClient.
// Single-window, single-instance app - this is safe and simple.
// ─────────────────────────────────────────────────────────────────────────────
static AyamaLogicNode*           g_logic            = nullptr;
static ayama_ui::AgentLauncher*  g_launcher         = nullptr;
// Session-only disclaimer acceptance - deliberately NOT persisted, the
// modal must appear on every launch so users never "auto-accept by inertia"
// terms they last saw weeks ago.
static bool                      g_session_accepted = false;

// ─────────────────────────────────────────────────────────────────────────────
// draw_widgets - root render function (called by RenderNode each frame)
// ─────────────────────────────────────────────────────────────────────────────
static void draw_widgets(const AyamaAppState& s) noexcept
{
    // ── AntiCheat disclaimer (every-launch) ──────────────────────────────────
    // Blocks all other interaction until the user acknowledges. Re-shown on
    // every launch as a deliberate friction step - there is no persistent
    // "I accepted before" path. Quit terminates the process; we use
    // std::exit because the render lambda has no clean handle on GLFW.
    {
        bool quit_requested = false;
        const bool modal_open = ayama_ui::draw_anticheat_warning_modal(
            g_session_accepted, quit_requested);
        if (quit_requested) {
            // Job Object's KILL_ON_JOB_CLOSE still tears down the agent.
            std::exit(0);
        }
        if (modal_open) {
            return;  // skip the rest of the UI while the modal is up
        }
    }

    // Obtain AyamaClient pointer from logic node (non-owning, always valid
    // while the graph is running).
    const ayama::ipc::AyamaClient* ac =
        g_logic ? &g_logic->client() : nullptr;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    constexpr float kStatusH = 26.f;

    // ── Status bar ────────────────────────────────────────────────────────────
    ImGui::SetNextWindowPos(
        ImVec2{vp->WorkPos.x,
               vp->WorkPos.y + vp->WorkSize.y - kStatusH});
    ImGui::SetNextWindowSize(ImVec2{vp->WorkSize.x, kStatusH});

    constexpr ImGuiWindowFlags kBarFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.10f, 0.10f, 0.10f, 1.f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2{8.f, 4.f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);

    if (ImGui::Begin("##ayama_status", nullptr, kBarFlags)) {
        // Agent health indicator. We surface three distinct states so the
        // user can tell apart "agent not running at all" from "agent ran
        // but stalled out". Stale = SHM connected but last_sync_tsc is
        // more than ~2 seconds old, OR the agent's own watchdog flag is
        // tripped. Reads only fields already published by AyamaLogicNode.
        const bool connected = s.snap.agent_connected != 0u;
        const uint64_t now_tsc = phyriad::hal::rdtsc();
        const uint64_t tsc_freq = phyriad::hal::calibrate_tsc_freq();
        const uint64_t stale_threshold_tsc = (tsc_freq > 0u)
            ? tsc_freq * 2u                   // 2 seconds
            : 6'000'000'000ull;               // ~2 s @ 3 GHz fallback
        const bool stale =
            connected &&
            (s.snap.watchdog_ok == 0u ||
             (s.snap.last_sync_tsc != 0u &&
              (now_tsc - s.snap.last_sync_tsc) > stale_threshold_tsc));
        const char* agent_label;
        ImVec4      agent_col;
        const char* agent_tip;
        if (!connected) {
            agent_label = "[--] Agent: down";
            agent_col   = ImVec4{0.9f, 0.4f, 0.2f, 1.f};
            agent_tip   = "No connection to ayama-agent. The UI will keep "
                          "polling; close and reopen if this persists.";
        } else if (stale) {
            agent_label = "[!!] Agent: stalled";
            agent_col   = ImVec4{0.95f, 0.55f, 0.20f, 1.f};
            agent_tip   = "The agent SHM is mapped but the heartbeat is "
                          "stale (>2 s). The agent's own watchdog may have "
                          "tripped - close and reopen to respawn it.";
        } else {
            agent_label = "[OK] Agent: live";
            agent_col   = ImVec4{0.30f, 0.90f, 0.40f, 1.f};
            agent_tip   = "Agent is running and its internal watchdog is "
                          "ticking. Telemetry is fresh.";
        }
        ImGui::PushStyleColor(ImGuiCol_Text, agent_col);
        ImGui::TextUnformatted(agent_label);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", agent_tip);

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Optimization gating indicator. Always visible so the user can
        // tell at a glance whether Ayama is actively modifying processes
        // (ACTIVE / green) or just observing (PAUSED / amber). Click
        // Start/Pause/Reset on the Dashboard to toggle.
        if (s.snap.policies_paused) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.80f, 0.20f, 1.f});
            ImGui::TextUnformatted("[||] Paused");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Optimizations are NOT being applied.\n"
                    "Click Start on the Dashboard to begin.");
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.30f, 0.90f, 0.40f, 1.f});
            ImGui::TextUnformatted("[>>] Active");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Optimizations are being applied to detected games.\n"
                    "Click Pause or Reset on the Dashboard to stop.");
            }
        }

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // AntiCheat-aware reminder badge. Always visible - keeps the
        // safety responsibility in front of the user without being a popup.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.78f, 0.20f, 1.f});
        ImGui::TextUnformatted("[!] AC-aware");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Ayama modifies process priority/affinity at the OS level.\n"
                "Disable Ayama (or exclude the game in Targets) before\n"
                "launching titles with kernel-level anticheat: Vanguard,\n"
                "EAC, BattlEye, Ricochet.");
        }

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Auto-launched indicator shown when ayama-ui spawned its own
        // background agent (vs. user starting ayama-agent.exe manually).
        if (g_launcher && g_launcher->spawned()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.55f, 0.80f, 1.0f, 1.f});
            ImGui::TextUnformatted("[auto]");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Agent was launched automatically by the UI.\n"
                    "It will be terminated when you close this window.");
            }
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
        }

        // Detected CPU architecture badge - set once at startup, lets users
        // confirm at a glance which strategy Ayama is using.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.65f, 0.85f, 1.0f, 1.f});
        ImGui::Text("%s", ayama_ui::arch_short_label(s.snap.cpu_class));
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Process breakdown by TargetKind. "Games: N" alone was
        // misleading because `target_count` includes every observed
        // process — browser tabs, Discord, OBS, the agent itself's PID,
        // etc. — so a developer with a typical desktop would see
        // "Games: 38" without a single game running. Walk the real
        // targets array and split by kind.
        uint32_t n_games = 0u, n_apps = 0u, n_watched = 0u;
        if (g_logic && g_logic->client().is_connected()) {
            for (const auto& t : g_logic->client().targets()) {
                switch (t.kind) {
                    case ayama::observer::TargetKind::Game:
                        ++n_games; break;
                    case ayama::observer::TargetKind::Stream:
                    case ayama::observer::TargetKind::Comm:
                    case ayama::observer::TargetKind::Browser:
                    case ayama::observer::TargetKind::Productivity:
                        ++n_apps; break;
                    case ayama::observer::TargetKind::Unknown:
                        ++n_watched; break;
                    case ayama::observer::TargetKind::System:
                        break;
                }
            }
        }

        if (n_games + n_apps + n_watched == 0u) {
            ImGui::TextDisabled("No processes observed");
        } else {
            // Games count first, coloured by whether any are present —
            // this is the metric the user actually cares about.
            const ImVec4 gcol = (n_games > 0u)
                ? ImVec4{0.55f, 0.95f, 0.65f, 1.f}
                : ImVec4{0.55f, 0.55f, 0.55f, 1.f};
            ImGui::PushStyleColor(ImGuiCol_Text, gcol);
            ImGui::Text("Games: %u", n_games);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("| Apps: %u | Watched: %u", n_apps, n_watched);

            // Migration pressure only meaningful on asymmetric CPUs and
            // only when at least one game is active (the metric averages
            // over the games' migrations_per_sec).
            if (n_games > 0u && ayama_ui::is_asymmetric(s.snap.cpu_class)) {
                const float p = s.snap.aggregate_pressure;
                const ImVec4 pcol = (p >= 1.5f) ? ImVec4{0.95f,0.30f,0.25f,1.f}
                                  : (p >= 0.8f) ? ImVec4{0.95f,0.80f,0.20f,1.f}
                                                : ImVec4{0.30f,0.90f,0.40f,1.f};
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, pcol);
                ImGui::Text("| Pressure: %.2f", p);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Cross-CCD migration pressure (0.0..2.0):\n"
                        "  <0.8 - low (normal scheduling)\n"
                        "  0.8-1.5 - elevated\n"
                        "  >=1.5 - severe (Ayama actively counteracting)");
                }
            }
        }

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Agent self-resources. Populated from SelfMonitor → SHM
        // (previously these were always 0 because nothing wrote to
        // the AyamaAppState::self_* fields). Colour-coded against the
        // anti-parasitic budget: green <1%, amber 1-5%, red >5% CPU.
        const float cpu = s.self_cpu_pct;
        const ImVec4 ccol = (cpu > 5.0f) ? ImVec4{0.95f,0.30f,0.25f,1.f}
                          : (cpu > 1.0f) ? ImVec4{0.95f,0.80f,0.20f,1.f}
                                         : ImVec4{0.55f,0.55f,0.55f,1.f};
        ImGui::PushStyleColor(ImGuiCol_Text, ccol);
        ImGui::Text("agent: %.2f%%", cpu);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f MB", s.self_rss_mb);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Ayama agent's own CPU and resident memory.\n"
                "Budget: <1%% idle, <5%% active, <50 MB RSS.\n"
                "Sampled every ~500 ms.");
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // ── Main tabbed window ────────────────────────────────────────────────────
    ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2{vp->WorkSize.x, vp->WorkSize.y - kStatusH},
        ImGuiCond_Always);

    constexpr ImGuiWindowFlags kMainFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove  |
        ImGuiWindowFlags_NoResize     | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2{8.f, 8.f});

    if (ImGui::Begin("##ayama_main", nullptr, kMainFlags)) {
        if (ImGui::BeginTabBar("##ayama_tabs")) {

            // Auto-focus Setup tab on first launch (when agent not yet
            // connected) so new users see the onboarding flow. After the
            // agent connects, the natural default flips to Dashboard.
            const bool first_run =
                !s.snap.agent_connected ||
                s.snap.cpu_class == ayama_ui::kCpuUnknown;
            const ImGuiTabItemFlags setup_flags = first_run
                ? ImGuiTabItemFlags_SetSelected
                : ImGuiTabItemFlags_None;

            if (ImGui::BeginTabItem("Dashboard")) {
                draw_dashboard_panel(s, ac);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Setup", nullptr, setup_flags)) {
                draw_setup_panel(s, ac);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Targets")) {
                draw_targets_panel(s, ac);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Policies")) {
                draw_policies_panel(s, ac);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Actions")) {
                draw_actions_panel(s, ac);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Benchmark")) {
                draw_bench_panel(s, ac);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Advanced")) {
                draw_advanced_panel(s, ac);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) {
                ayama_ui::draw_settings_panel();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    // ── Auto-spawn ayama-agent if it isn't already running ───────────────────
    // Outlives Application::run so the Job Object stays alive for the
    // process lifetime; ~AgentLauncher tears the spawned agent down on a
    // clean exit, KILL_ON_JOB_CLOSE handles crashes.
    ayama_ui::AgentLauncher launcher;
    if (!launcher.start()) {
        std::fprintf(stderr,
            "[ayama-ui] Could not auto-launch ayama-agent - UI will run in "
            "disconnected mode until the agent is started manually.\n");
    }
    g_launcher = &launcher;

    phyriad::ui::ApplicationConfig cfg;
    cfg.window.title         = "Ayama - Runtime Optimizer";
    cfg.window.width         = 1280;
    cfg.window.height        = 800;
    cfg.window.resizable     = true;
    cfg.frame_arena_capacity = 2u * 1024u * 1024u;  // 2 MB (smaller than std_window)
    cfg.profile              = phyriad::ProfileKind::BALANCED;
    // Note: Ayama-ui talks to ayama-agent via shared memory (AyamaClient),
    // not via a daemon process. The former Phyriad daemon pillar has been
    // retired - see AYAMA_MASTER_PLAN.md changelog.

    return phyriad::ui::Application::run(
        cfg,
        [](phyriad::api::NodeRegistry&      reg,
           phyriad::api::DslGraphBuilder&   builder,
           phyriad::render::IRenderBackend& backend,
           phyriad::render::FrameArena&     arena)
        {
            // ── AyamaLogicNode ────────────────────────────────────────────────
            // Pre-created (not default-constructible in general) → wrap.
            auto* logic = new AyamaLogicNode();
            g_logic = logic;  // expose to draw_widgets for AyamaClient access

            reg.register_factory(
                "logic",
                [logic](phyriad::NodeId id) noexcept -> phyriad::api::NodeHandle {
                    return phyriad::api::NodeHandle::wrap(logic, id);
                },
                phyriad::schema::schema_hash<AyamaAppState>(),
                phyriad::schema::schema_hash<phyriad::ui::InputEvent>()
            );

            reg.wire_registry().register_type<AyamaAppState>();

            // ── RenderNode<AyamaAppState> ─────────────────────────────────────
            using RN = phyriad::ui::RenderNode<AyamaAppState>;
            auto* rn = new RN(backend, arena, &draw_widgets);

            reg.register_factory(
                "render",
                [rn](phyriad::NodeId id) noexcept -> phyriad::api::NodeHandle {
                    return phyriad::api::NodeHandle::wrap(rn, id);
                },
                phyriad::schema::schema_hash<phyriad::render::RenderStats>(),
                phyriad::schema::schema_hash<AyamaAppState>()
            );

            // ── Nodes in topological order ────────────────────────────────────
            builder
                .node<AyamaLogicNode>("logic", phyriad::api::placement::logic())
                .node<RN>            ("render", phyriad::api::placement::ui_main());

            // ── Wires ─────────────────────────────────────────────────────────
            // ui outlet 0 (InputEvent)  → logic inlet 0
            // ui outlet 1 (WindowState) → logic inlet 1
            // logic outlet 0 (AyamaAppState) → render inlet 1
            (void)builder.wire("ui",    0).to("logic",  0);
            (void)builder.wire("ui",    1).to("logic",  1);
            (void)builder.wire("logic", 0).to("render", 1);
        }
    );
}
// Made with my soul - Swately <3
