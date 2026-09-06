#include "GameViewPanel.h"
#include "EditorUIHelpers.h"
#include "EditorSettings.h"
#include <IconsFontAwesome6.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>

void GameViewPanel::SetPreset(const ResolutionPreset& preset) {
    m_CurrentPreset = preset;
    SaveSettings();
}

void GameViewPanel::OnPlayStateChanged(bool isPlaying) {
    if (isPlaying && m_MaximizeOnPlay) {
        m_FullscreenRequestPending = true;
        m_FullscreenRequestValue = true;
        m_FullscreenFromMaximizeOnPlay = true;
    } else if (!isPlaying && m_FullscreenFromMaximizeOnPlay) {
        m_FullscreenRequestPending = true;
        m_FullscreenRequestValue = false;
        m_FullscreenFromMaximizeOnPlay = false;
    }
}

bool GameViewPanel::ConsumeFullscreenRequest(bool& outWantFullscreen) {
    if (!m_FullscreenRequestPending) return false;
    outWantFullscreen = m_FullscreenRequestValue;
    m_FullscreenRequestPending = false;
    return true;
}

void GameViewPanel::ComputeTargetSize(ImVec2 available, int& outWidth, int& outHeight) const {
    switch (m_CurrentPreset.Mode) {
        case AspectRatioMode::FixedResolution:
            outWidth = m_CurrentPreset.Width;
            outHeight = m_CurrentPreset.Height;
            break;
        case AspectRatioMode::FixedAspect: {
            ResolutionManager::LetterboxRect rect = ResolutionManager::CalculateLetterboxRect(available, m_CurrentPreset.AspectRatio);
            outWidth = (int)rect.Size.x;
            outHeight = (int)rect.Size.y;
            break;
        }
        case AspectRatioMode::FreeAspect:
        default:
            outWidth = (int)available.x;
            outHeight = (int)available.y;
            break;
    }
    outWidth = std::max(outWidth, 1);
    outHeight = std::max(outHeight, 1);
}

void GameViewPanel::DrawToolbar() {
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("##GameViewPreset", m_CurrentPreset.Label.c_str())) {
        for (const auto& preset : ResolutionManager::BuiltInPresets()) {
            if (ImGui::Selectable(preset.Label.c_str(), preset.Label == m_CurrentPreset.Label)) {
                SetPreset(preset);
            }
        }
        for (const auto& preset : m_CustomPresets) {
            if (ImGui::Selectable(preset.Label.c_str(), preset.Label == m_CurrentPreset.Label)) {
                SetPreset(preset);
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable(ICON_FA_PLUS "  Add Custom Aspect/Resolution...")) {
            m_ShowCustomModal = true;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        EditorUI::SetTooltip("Locks the Game View (and Play Mode) to this aspect ratio or exact\nresolution, letterboxing the rest. Free Aspect fills whatever space is available.");
    }

    ImGui::SameLine();
    if (ImGui::Checkbox("Maximize on Play", &m_MaximizeOnPlay)) SaveSettings();
    if (ImGui::IsItemHovered()) {
        EditorUI::SetTooltip("When enabled, entering Play Mode automatically expands the game\nto fullscreen instead of staying windowed.");
    }

    ImGui::SameLine();
    if (ImGui::Button(m_LastKnownOsFullscreen ? ICON_FA_COMPRESS "  Windowed" : ICON_FA_EXPAND "  Fullscreen")) {
        m_FullscreenRequestPending = true;
        m_FullscreenRequestValue = !m_LastKnownOsFullscreen;
    }
    if (ImGui::IsItemHovered()) {
        EditorUI::SetTooltip("Expand the game to a true borderless fullscreen window (F11).");
    }

    ImGui::SameLine();
    if (ImGui::Checkbox(ICON_FA_GAUGE_HIGH "  Stats", &m_ShowStatsOverlay)) SaveSettings();
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Show FPS, frame time, draw calls, and vert/tri counts over the view.");
}

void GameViewPanel::DrawCustomResolutionModal() {
    if (m_ShowCustomModal) {
        ImGui::OpenPopup("Custom Resolution");
        m_ShowCustomModal = false;
    }
    if (ImGui::BeginPopupModal("Custom Resolution", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputInt("Width", &m_CustomWidth);
        ImGui::InputInt("Height", &m_CustomHeight);
        m_CustomWidth = std::max(m_CustomWidth, 1);
        m_CustomHeight = std::max(m_CustomHeight, 1);

        ImGui::Spacing();
        if (ImGui::Button("Add", ImVec2(120.0f, 0.0f))) {
            ResolutionPreset preset;
            preset.Label = std::to_string(m_CustomWidth) + "x" + std::to_string(m_CustomHeight) + " Custom";
            preset.Mode = AspectRatioMode::FixedResolution;
            preset.Width = m_CustomWidth;
            preset.Height = m_CustomHeight;
            preset.AspectRatio = (float)m_CustomWidth / (float)m_CustomHeight;
            m_CustomPresets.push_back(preset);
            SetPreset(preset);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void GameViewPanel::RenderUI(const GameViewStats* stats, bool isOsFullscreen, bool playing, bool inputEngaged,
                            bool noSceneCamera) {
    m_LastKnownOsFullscreen = isOsFullscreen;
    m_ViewHovered = false;

    // NoFocusOnAppearing: Game goes unsubmitted for the whole duration of Play Mode, which makes
    // it "just activated by user" the instant it's Begin()'d again after Stop - without this
    // flag, ImGui auto-focuses it purely for having reappeared, which a separate docking code
    // path then reads to force it back to being the active tab over Scene every single time,
    // regardless of anything EditorLayer explicitly requests afterward. See EditorLayer's
    // matching flag on "Scene" for the full explanation (found in ImGui's own source, not
    // guessed) - both windows need it, since either one's auto-focus alone can win this fight.
    // The dock tab bar renders synchronously inside Begin(); on a light-chrome theme (Windows XP)
    // its text — tab labels, per-tab ×, the ▼ list button, node × — wants to stay white against
    // the coloured tabs. Detected from WindowBg luminance so the dark themes are a no-op. The
    // body below draws in the theme's normal text colour, no wrapping needed.
    const ImVec4 gvBg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const bool gvLightChrome = (0.299f * gvBg.x + 0.587f * gvBg.y + 0.114f * gvBg.z) > 0.5f;
    if (gvLightChrome) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.98f, 1.00f, 1.0f));
    m_Visible = ImGui::Begin("Game", &m_WindowOpen, ImGuiWindowFlags_NoFocusOnAppearing);
    if (gvLightChrome) ImGui::PopStyleColor();
    if (!m_Visible) {
        m_LastAvailableRegion = ImVec2(0.0f, 0.0f);
        m_ViewImageSize = ImVec2(0.0f, 0.0f);
        ImGui::End();
        return;
    }

    DrawToolbar();
    ImGui::Separator();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    m_LastAvailableRegion = avail;
    m_ViewImageSize = ImVec2(0.0f, 0.0f);

    if (avail.x > 1.0f && avail.y > 1.0f && m_Framebuffer.IsValid()) {
        float targetAspect = m_CurrentPreset.Mode == AspectRatioMode::FreeAspect ? -1.0f : m_CurrentPreset.AspectRatio;
        ResolutionManager::LetterboxRect rect = ResolutionManager::CalculateLetterboxRect(avail, targetAspect);

        ImVec2 containerStart = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Explicit black bars rather than leaving the panel's own background showing through -
        // reads as an intentional letterbox/pillarbox instead of an unrendered gap.
        dl->AddRectFilled(containerStart, ImVec2(containerStart.x + avail.x, containerStart.y + avail.y), IM_COL32(0, 0, 0, 255));

        ImVec2 imagePos(containerStart.x + rect.Offset.x, containerStart.y + rect.Offset.y);
        m_ViewImagePos = imagePos;
        m_ViewImageSize = rect.Size;
        ImGui::SetCursorScreenPos(imagePos);
        // uv0=(0,1)/uv1=(1,0): OpenGL textures are bottom-left origin, ImGui::Image expects
        // top-left, so this flips the framebuffer's color attachment right-side up.
        ImGui::Image((ImTextureID)(intptr_t)m_Framebuffer.ColorTexture(), rect.Size, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        m_ViewHovered = ImGui::IsItemHovered();

        // In-panel play: first click inside the running view captures mouse/keyboard for the
        // game; until then, a hint sits over the image. (Esc releases — handled in main.cpp.)
        if (playing && !inputEngaged) {
            if (m_ViewHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                m_EngageClickPending = true;
            }
            const char* hint = "Click to control  \xE2\x80\xA2  Esc to release";
            ImVec2 ts = ImGui::CalcTextSize(hint);
            ImVec2 anchor(imagePos.x + (rect.Size.x - ts.x) * 0.5f,
                          imagePos.y + rect.Size.y - ts.y - 16.0f);
            dl->AddRectFilled(ImVec2(anchor.x - 10.0f, anchor.y - 6.0f),
                              ImVec2(anchor.x + ts.x + 10.0f, anchor.y + ts.y + 6.0f),
                              IM_COL32(0, 0, 0, 150), 4.0f);
            dl->AddText(anchor, IM_COL32(255, 255, 255, 230), hint);
        } else if (!playing && noSceneCamera) {
            const char* hint = "No Camera in scene  \xE2\x80\xA2  previewing the editor view.  Add \xE2\x96\xB8 Camera to place one.";
            ImVec2 ts = ImGui::CalcTextSize(hint);
            ImVec2 anchor(imagePos.x + (rect.Size.x - ts.x) * 0.5f,
                          imagePos.y + rect.Size.y - ts.y - 14.0f);
            dl->AddRectFilled(ImVec2(anchor.x - 10.0f, anchor.y - 6.0f),
                              ImVec2(anchor.x + ts.x + 10.0f, anchor.y + ts.y + 6.0f),
                              IM_COL32(0, 0, 0, 140), 4.0f);
            dl->AddText(anchor, IM_COL32(230, 230, 230, 210), hint);
        }
        // (The persistent "Esc to release the cursor" banner was removed — that binding now
        // lives in Preferences ▸ Shortcuts like every other key.)

        if (m_ShowStatsOverlay && stats) {
            ImVec2 statsPos(imagePos.x + 8.0f, imagePos.y + 8.0f);
            char buf[192];
            snprintf(buf, sizeof(buf), "%d FPS (%.2f ms)\n%d draw calls\n%d tris / %d verts",
                stats->FPS, stats->FrameMs, stats->DrawCalls, stats->Triangles, stats->Vertices);
            ImVec2 textSize = ImGui::CalcTextSize(buf);
            dl->AddRectFilled(statsPos, ImVec2(statsPos.x + textSize.x + 12.0f, statsPos.y + textSize.y + 8.0f), IM_COL32(0, 0, 0, 140), 3.0f);
            dl->AddText(ImVec2(statsPos.x + 6.0f, statsPos.y + 4.0f), IM_COL32(255, 255, 255, 255), buf);
        }

        ImGui::SetCursorScreenPos(ImVec2(containerStart.x, containerStart.y + avail.y));
        // Submit a zero-size item so ImGui registers the extended content bounds - without a
        // trailing item after SetCursorScreenPos() its error-recovery check flags this as a bug.
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    ImGui::End();
    DrawCustomResolutionModal();
}

void GameViewPanel::LoadSettings() {
    const EditorSettings& settings = EditorSettings::Get();
    m_MaximizeOnPlay = settings.GameViewMaximizeOnPlay;
    m_ShowStatsOverlay = settings.GameViewShowStats;

    for (const auto& preset : ResolutionManager::BuiltInPresets()) {
        if (preset.Label == settings.GameViewPresetLabel) { m_CurrentPreset = preset; return; }
    }
    // Not a built-in — was a saved custom resolution. Re-derive it from the saved dimensions
    // rather than persisting the whole custom-preset list for just this one case.
    if (settings.GameViewPresetWidth > 0 && settings.GameViewPresetHeight > 0) {
        ResolutionPreset preset;
        preset.Label = settings.GameViewPresetLabel;
        preset.Mode = AspectRatioMode::FixedResolution;
        preset.Width = settings.GameViewPresetWidth;
        preset.Height = settings.GameViewPresetHeight;
        preset.AspectRatio = (float)preset.Width / (float)preset.Height;
        m_CustomPresets.push_back(preset);
        m_CurrentPreset = preset;
    }
}

void GameViewPanel::SaveSettings() const {
    EditorSettings& settings = EditorSettings::Get();
    settings.GameViewMaximizeOnPlay = m_MaximizeOnPlay;
    settings.GameViewShowStats = m_ShowStatsOverlay;
    settings.GameViewPresetLabel = m_CurrentPreset.Label;
    settings.GameViewPresetWidth = m_CurrentPreset.Mode == AspectRatioMode::FixedResolution ? m_CurrentPreset.Width : 0;
    settings.GameViewPresetHeight = m_CurrentPreset.Mode == AspectRatioMode::FixedResolution ? m_CurrentPreset.Height : 0;
    EditorSettings::Save();
}
