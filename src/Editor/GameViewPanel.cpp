#include "GameViewPanel.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "EditorSettings.h"
#include <IconsFontAwesome6.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

void GameViewPanel::SetPreset(const ResolutionPreset& preset) {
    m_CurrentPreset = preset;
    SaveSettings();
}

void GameViewPanel::OnPlayStateChanged(bool isPlaying) {
    // Read the setting live (it's edited in Preferences > Viewport now, not a panel checkbox).
    if (isPlaying && EditorSettings::Get().GameViewMaximizeOnPlay) {
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

void GameViewPanel::DrawAspectControl() {
    const float itemW = 200.0f;
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    // #54 — used to take a `contrast` param (sampled scene luminance) and invert text/plate
    // between light-on-dark and dark-on-light. Fixed opaque plate + fixed light text now,
    // matching the other Game-view overlays.
    const ImU32 textCol = EditorUIPrimitives::kHudTextColor;

    // Custom button + manual popup rather than BeginCombo/ImGui's own auto-placement, so the
    // caret can match the popup's actual open direction below.
    ImGui::PushStyleColor(ImGuiCol_Button,        EditorUIPrimitives::kHudPlateColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 40, 40, 220));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(50, 50, 50, 230));
    ImGui::PushStyleColor(ImGuiCol_Text,          textCol);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f)); // left-align like a combo
    if (ImGui::Button((m_CurrentPreset.Label + "###aspectbtn").c_str(), ImVec2(itemW, h)))
        ImGui::OpenPopup("##AspectPopup");
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    const bool btnHovered = ImGui::IsItemHovered();
    m_AspectComboOpen = ImGui::IsPopupOpen("##AspectPopup");

    // Caret at the button's right edge — up while the list is open, down otherwise.
    {
        const float cx = p0.x + itemW - h * 0.5f;
        const float cy = p0.y + h * 0.5f;
        const float r = ImGui::GetFontSize() * 0.20f;
        const ImU32 col = textCol;
        ImDrawList* d = ImGui::GetWindowDrawList();
        if (m_AspectComboOpen)
            d->AddTriangleFilled(ImVec2(cx - r, cy + r * 0.7f), ImVec2(cx + r, cy + r * 0.7f), ImVec2(cx, cy - r * 0.9f), col);
        else
            d->AddTriangleFilled(ImVec2(cx - r, cy - r * 0.7f), ImVec2(cx + r, cy - r * 0.7f), ImVec2(cx, cy + r * 0.9f), col);
    }
    if (btnHovered) {
        EditorUI::SetTooltip("Locks the Game View (and Play Mode) to this aspect ratio or exact\nresolution, letterboxing the rest. Free Aspect fills whatever space is available.");
    }

    ImGui::SetNextWindowPos(ImVec2(p0.x, p0.y + h + 2.0f), ImGuiCond_Always, ImVec2(0.0f, 0.0f)); // grow downward, below the toolbar button
    ImGui::SetNextWindowSizeConstraints(ImVec2(itemW, 0.0f), ImVec2(itemW, 600.0f));
    if (ImGui::BeginPopup("##AspectPopup")) {
        auto row = [&](const ResolutionPreset& preset) {
            if (ImGui::Selectable(preset.Label.c_str(), preset.Label == m_CurrentPreset.Label))
                SetPreset(preset);
        };
        for (const auto& preset : ResolutionManager::BuiltInPresets()) row(preset);
        for (const auto& preset : m_CustomPresets) row(preset);
        ImGui::Separator();
        if (ImGui::Selectable(ICON_FA_PLUS "  Add Custom Aspect/Resolution..."))
            m_ShowCustomModal = true;
        ImGui::EndPopup();
    }
    // #140 — the Game view's own stats overlay had no toggle anywhere (the toolbar Stats button
    // drives the Scene-side Statistics panel), so it could never be turned off.
    ImGui::SameLine(0.0f, 6.0f);
    {
        bool showStats = EditorSettings::Get().GameViewShowStats;
        ImGui::PushStyleColor(ImGuiCol_Button, EditorUIPrimitives::kHudPlateColor);
        ImGui::PushStyleColor(ImGuiCol_Text, showStats ? EditorUIPrimitives::kHudTextColor
                                                        : ImGui::GetColorU32(ImGuiCol_TextDisabled));
        if (ImGui::Button(ICON_FA_CHART_SIMPLE "##gvstats", ImVec2(h, h))) {
            EditorSettings::Get().GameViewShowStats = !showStats;
            EditorSettings::Save();
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip(showStats ? "Hide the Game view stats overlay"
                                                                   : "Show the Game view stats overlay");
    }
    // Fullscreen dropped from here — it's in the viewport transport overlay and on F11.
    // "Maximize on Play" -> Preferences > Viewport; Stats overlay -> the main toolbar Stats button.
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
    // This used to flip the dock tab bar's text to white against Windows XP's saturated-green
    // tab chrome (detected from WindowBg luminance). Phase 1 item 9 removed that theme; Light's
    // tabs are neutral and already pair with its own normal text colour, so the luminance check
    // would be a false positive there now (see EditorLayerInternal.h's PanelChromeIsLight) —
    // permanently disabled instead.
    const bool gvLightChrome = false;
    if (gvLightChrome) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.98f, 1.00f, 1.0f));
    m_Visible = ImGui::Begin("Game", &m_WindowOpen, ImGuiWindowFlags_NoFocusOnAppearing);
    if (gvLightChrome) ImGui::PopStyleColor();
    if (!m_Visible) {
        m_LastAvailableRegion = ImVec2(0.0f, 0.0f);
        m_ViewImageSize = ImVec2(0.0f, 0.0f);
        ImGui::End();
        return;
    }

    // #29 — a real toolbar row (in normal ImGui layout flow, like the Asset Browser's own
    // toolbar strip) instead of the aspect/resolution control floating as a bottom-left overlay
    // on top of the rendered image. The control's own styling/popup is unchanged; only where it's
    // drawn from moved.
    ImGui::BeginChild("##GameToolbar", ImVec2(0.0f, ImGui::GetFrameHeight()), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawAspectControl();
    ImGui::EndChild();

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

        // #54 — used to sample the rendered image's luminance behind the top-left and bottom
        // overlays (throttled ~10 Hz, eased) and steer them between light-on-dark and
        // dark-on-light. Fixed opaque plates + fixed light text are legible over anything.

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
            EditorUIPrimitives::DrawHudPlate(dl, ImVec2(anchor.x - 10.0f, anchor.y - 6.0f),
                              ImVec2(anchor.x + ts.x + 10.0f, anchor.y + ts.y + 6.0f), 4.0f);
            dl->AddText(anchor, EditorUIPrimitives::kHudTextColor, hint);
        } else if (!playing && noSceneCamera) {
            const char* hint = "No Camera in scene - previewing the editor view. Create " ICON_FA_ANGLE_RIGHT " Camera to place one.";
            ImVec2 ts = ImGui::CalcTextSize(hint);
            ImVec2 anchor(imagePos.x + (rect.Size.x - ts.x) * 0.5f,
                          imagePos.y + rect.Size.y - ts.y - 14.0f);
            EditorUIPrimitives::DrawHudPlate(dl, ImVec2(anchor.x - 10.0f, anchor.y - 6.0f),
                              ImVec2(anchor.x + ts.x + 10.0f, anchor.y + ts.y + 6.0f), 4.0f);
            dl->AddText(anchor, EditorUIPrimitives::kHudTextColor, hint);
        }
        // (The persistent "Esc to release the cursor" banner was removed — that binding now
        // lives in Preferences ▸ Shortcuts like every other key.)

        if (EditorSettings::Get().GameViewShowStats && stats) {
            ImVec2 statsPos(imagePos.x + 8.0f, imagePos.y + 8.0f);
            char buf[192];
            snprintf(buf, sizeof(buf), "%d FPS (%.2f ms)\n%d draw calls\n%d tris / %d verts",
                stats->FPS, stats->FrameMs, stats->DrawCalls, stats->Triangles, stats->Vertices);
            ImVec2 textSize = ImGui::CalcTextSize(buf);
            EditorUIPrimitives::DrawHudPlate(dl, statsPos,
                ImVec2(statsPos.x + textSize.x + 12.0f, statsPos.y + textSize.y + 8.0f), 3.0f);
            dl->AddText(ImVec2(statsPos.x + 6.0f, statsPos.y + 4.0f), EditorUIPrimitives::kHudTextColor, buf);
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
    // #140 — Maximize on Play / Game stats used to be written back from copies cached at
    // startup, so changing the aspect preset silently reverted a preference changed since in
    // Preferences. Both are read live from EditorSettings now and only written where edited.
    settings.GameViewPresetLabel = m_CurrentPreset.Label;
    settings.GameViewPresetWidth = m_CurrentPreset.Mode == AspectRatioMode::FixedResolution ? m_CurrentPreset.Width : 0;
    settings.GameViewPresetHeight = m_CurrentPreset.Mode == AspectRatioMode::FixedResolution ? m_CurrentPreset.Height : 0;
    EditorSettings::Save();
}
