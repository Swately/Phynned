// tools/phynned-ui/widgets/anticheat_warning.hpp
// AntiCheat compatibility warning modal - shown once on first launch and
// every time the disclaimer text materially changes (version bump in
// UiPreferences::kDisclaimerCurrentVersion).
//
// Behaviour:
//   - Renders a modal that blocks all other UI until the user accepts.
//   - "I understand and accept the risk" → records the accepted version in
//     UiPreferences and dismisses the modal.
//   - "Quit" / window-close → caller is expected to exit the app (we set
//     out_quit = true so main can handle the GLFW close).
//
// The modal text is intentionally specific about kernel-level anticheat
// systems and lists known competitive titles so a user can't claim they
// weren't warned - this is both an ethical obligation and a defensive
// posture for the project.
//
#pragma once

#include <imgui.h>

namespace phynned_ui {

// Returns true while the modal is open (caller should NOT render the rest
// of the app's interactable widgets behind it - ImGui's BeginPopupModal
// already greys out the background, so this is mostly for the launcher to
// know whether the user has finished onboarding).
//
// `session_accepted` is a session-only flag (not persisted): the modal
// must appear on EVERY application launch as a deliberate friction step,
// regardless of any prior acceptance. Once the user clicks accept in this
// session, the flag flips to true and we stop showing the modal until the
// next launch.
//
// `out_quit` is set to true if the user closes the window without accepting.
inline bool draw_anticheat_warning_modal(bool& session_accepted,
                                         bool& out_quit) noexcept
{
    constexpr const char* kPopupId = "AntiCheat Compatibility - Read Before Use";

    if (!session_accepted) {
        ImGui::OpenPopup(kPopupId);
    }

    // Centre the modal over the main viewport.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2{vp->WorkPos.x + vp->WorkSize.x * 0.5f,
               vp->WorkPos.y + vp->WorkSize.y * 0.5f},
        ImGuiCond_Always,
        ImVec2{0.5f, 0.5f});
    ImGui::SetNextWindowSize(ImVec2{640.f, 0.f}, ImGuiCond_Always);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoCollapse       |
        ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::BeginPopupModal(kPopupId, nullptr, kFlags)) {
        return !session_accepted;
    }

    // ── Header ───────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.78f, 0.20f, 1.f});
    ImGui::TextUnformatted("[!] Important - AntiCheat Compatibility");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Body ─────────────────────────────────────────────────────────────────
    ImGui::TextWrapped(
        "Phynned improves game performance by adjusting standard OS settings "
        "for game processes: CPU affinity (which cores a process runs on) "
        "and scheduling priority. Phynned does NOT inject code into games, "
        "read game memory, or modify game files in any way.");
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.85f, 0.40f, 1.f});
    ImGui::TextWrapped(
        "However, some kernel-level anticheat systems may flag these "
        "OS-level adjustments as suspicious activity, and have been known "
        "to issue bans on that basis.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    ImGui::TextUnformatted("Known kernel-level anticheats to be cautious with:");
    ImGui::Bullet(); ImGui::TextUnformatted("Riot Vanguard      (Valorant, League of Legends)");
    ImGui::Bullet(); ImGui::TextUnformatted("Easy Anti-Cheat    (Fortnite, Apex Legends, R6 Siege, Elden Ring)");
    ImGui::Bullet(); ImGui::TextUnformatted("BattlEye           (PUBG, Rainbow Six, ARMA, DayZ)");
    ImGui::Bullet(); ImGui::TextUnformatted("Ricochet           (Call of Duty: MW2 / MW3 / Warzone)");
    ImGui::Bullet(); ImGui::TextUnformatted("BattlEye/Other     (Destiny 2, Rust, Escape From Tarkov)");
    ImGui::Spacing();

    ImGui::TextWrapped(
        "We recommend either: (a) disabling Phynned optimisations for the "
        "specific game's executable in the Targets tab, or (b) closing "
        "Phynned entirely before launching a competitive multiplayer match.");
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.65f, 0.85f, 1.0f, 1.f});
    ImGui::TextWrapped(
        "Single-player titles and games without kernel-mode anticheat are "
        "generally safe. Phynned is designed to be observable and reversible "
        "- all changes are listed in the Actions tab and you can revert "
        "them at any time.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Buttons ──────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.20f, 0.55f, 0.25f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.25f, 0.65f, 0.30f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.30f, 0.75f, 0.35f, 1.f});
    if (ImGui::Button("I understand and accept the risk", ImVec2{300.f, 32.f})) {
        session_accepted = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();

    if (ImGui::Button("Quit", ImVec2{80.f, 32.f})) {
        out_quit = true;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    return !session_accepted;
}

} // namespace phynned_ui
// Made with my soul - Swately <3
