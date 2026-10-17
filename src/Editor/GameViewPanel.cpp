#include "GameViewPanel.h"
#include "EditorTheme.h"
#include "EditorPanels.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "EditorSettings.h"
#include "EditorSound.h"
#include <IconsFontAwesome6.h>
#include <imgui_internal.h> // ImRect, FindWindowByName (the action bar's rect)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

void GameViewPanel::SetPreset(const ResolutionPreset& preset) {
    m_CurrentPreset = preset;
    m_Zoom = 0.0f;
    m_Pan = ImVec2(0.0f, 0.0f);
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
    const float itemW = 200.0f * EditorTheme::Scale();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    // #54 — used to take a `contrast` param (sampled scene luminance) and invert text/plate
    // between light-on-dark and dark-on-light. Fixed opaque plate + fixed light text now,
    // matching the other Game-view overlays.
    const ImU32 textCol = EditorUIPrimitives::kHudTextColor;

    // Custom button + manual popup rather than BeginCombo/ImGui's own auto-placement, so the
    // caret can match the popup's actual open direction below.
    ImGui::PushStyleColor(ImGuiCol_Button,        EditorUIPrimitives::kHudPlateColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  EditorTheme::Pressed);
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
            if (ImGui::Selectable(preset.Label.c_str(), preset.Label == m_CurrentPreset.Label)) {
                // The user's pick only (not SetPreset itself: loading settings and the custom modal's Add go through it too).
                if (preset.Label != m_CurrentPreset.Label)
                    EditorSound::Play(EditorSound::Event::ResolutionChange, EditorSound::LastItemScreenX());
                SetPreset(preset);
            }
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
    ImGui::SameLine(0.0f, 6.0f * EditorTheme::Scale());
    {
        bool showStats = EditorSettings::Get().GameViewShowStats;
        ImGui::PushStyleColor(ImGuiCol_Button, EditorUIPrimitives::kHudPlateColor);
        ImGui::PushStyleColor(ImGuiCol_Text, showStats ? EditorTheme::U32(EditorTheme::Accent)
                                                        : EditorTheme::U32(EditorTheme::Secondary));
        if (ImGui::Button(ICON_FA_CHART_SIMPLE "##gvstats", ImVec2(h, h))) {
            EditorSettings::Get().GameViewShowStats = !showStats;
            EditorSound::Play(showStats ? EditorSound::Event::StatsOff : EditorSound::Event::StatsOn, EditorSound::LastItemScreenX());
            EditorSettings::Save();
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip(showStats ? "Hide the Game view stats overlay"
                                                                   : "Show the Game view stats overlay");
    }
    ImGui::SameLine(0.0f, 10.0f * EditorTheme::Scale());
    DrawScaleControl();
    // Fullscreen dropped from here — it's in the viewport transport overlay and on F11.
    // "Maximize on Play" -> Preferences > Viewport; Stats overlay -> the main toolbar Stats button.
}

// Unity's Scale slider: from the smaller of fit and 1:1 up to 8x. At the fit end it reads "Fit" and
// follows the panel as it resizes; right-click returns to it.
void GameViewPanel::DrawScaleControl() {
    const float lo = std::min(m_FitScale, 1.0f), hi = 8.0f;
    float value = m_Zoom > 0.0f ? m_Zoom : m_FitScale;
    char format[16];
    std::snprintf(format, sizeof format, "%s", m_Zoom > 0.0f ? "%.2fx" : "Fit");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Scale");
    ImGui::SameLine(0.0f, 4.0f * EditorTheme::Scale());
    ImGui::SetNextItemWidth(140.0f * EditorTheme::Scale());
    if (ImGui::SliderFloat("##gvscale", &value, lo, hi, format, ImGuiSliderFlags_Logarithmic)) {
        m_Zoom = std::abs(value - m_FitScale) <= 0.01f * m_FitScale ? 0.0f : value;
        if (m_Zoom == 0.0f) m_Pan = ImVec2(0.0f, 0.0f);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { m_Zoom = 0.0f; m_Pan = ImVec2(0.0f, 0.0f); }
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Framebuffer pixels per screen pixel (1.00x = 1:1). Fit shows the whole frame.\n"
                             "Scroll over the view to zoom (while it isn't controlling the game), drag with\n"
                             "the middle mouse button to pan. Right-click here to go back to Fit.");
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
        if (EditorUIPrimitives::PrimaryButton("Add", ImVec2(120.0f * EditorTheme::Scale(), 0.0f))) {
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
        if (EditorUIPrimitives::SecondaryButton("Cancel", ImVec2(120.0f * EditorTheme::Scale(), 0.0f))) ImGui::CloseCurrentPopup();
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
    m_Visible = ImGui::Begin(EditorPanels::Game, &m_WindowOpen, ImGuiWindowFlags_NoFocusOnAppearing);
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

        // Scale: the fitted rect is the default; zoomed, the frame is drawn at framebuffer size x zoom,
        // centred (plus the pan) and clipped to the panel - only the part on screen is submitted, with
        // its matching UVs, so the item (hover, clicks) is exactly what's visible.
        const float fbW = (float)std::max(m_Framebuffer.Width(), 1), fbH = (float)std::max(m_Framebuffer.Height(), 1);
        m_FitScale = std::max(rect.Size.x / fbW, 1e-3f);
        const float scale = m_Zoom > 0.0f ? m_Zoom : m_FitScale;
        const ImVec2 shown(fbW * scale, fbH * scale);
        // Panned no further than the image's edge reaching the panel's (none along an axis it fits).
        m_Pan.x = std::clamp(m_Pan.x, -std::max(0.0f, (shown.x - avail.x) * 0.5f), std::max(0.0f, (shown.x - avail.x) * 0.5f));
        m_Pan.y = std::clamp(m_Pan.y, -std::max(0.0f, (shown.y - avail.y) * 0.5f), std::max(0.0f, (shown.y - avail.y) * 0.5f));
        const ImVec2 fullMin(containerStart.x + (avail.x - shown.x) * 0.5f + m_Pan.x,
                             containerStart.y + (avail.y - shown.y) * 0.5f + m_Pan.y);
        const ImVec2 visMin(std::max(fullMin.x, containerStart.x), std::max(fullMin.y, containerStart.y));
        const ImVec2 visMax(std::min(fullMin.x + shown.x, containerStart.x + avail.x),
                            std::min(fullMin.y + shown.y, containerStart.y + avail.y));
        const ImVec2 imagePos = visMin;
        rect.Size = ImVec2(std::max(visMax.x - visMin.x, 1.0f), std::max(visMax.y - visMin.y, 1.0f));
        m_ViewImagePos = imagePos;
        m_ViewImageSize = rect.Size;
        ImGui::SetCursorScreenPos(imagePos);
        // OpenGL textures are bottom-left origin and ImGui::Image expects top-left, so v runs 1 -> 0
        // down the image (the whole frame: uv0=(0,1), uv1=(1,0)).
        const ImVec2 uv0((visMin.x - fullMin.x) / shown.x, 1.0f - (visMin.y - fullMin.y) / shown.y);
        const ImVec2 uv1((visMax.x - fullMin.x) / shown.x, 1.0f - (visMax.y - fullMin.y) / shown.y);
        ImGui::Image((ImTextureID)(intptr_t)m_Framebuffer.ColorTexture(), rect.Size, uv0, uv1);
        m_ViewHovered = ImGui::IsItemHovered();

        // Zoom with the wheel about the cursor, pan with the middle button - not while the game has input.
        if (!(playing && inputEngaged)) {
            ImGuiIO& io = ImGui::GetIO();
            if (m_ViewHovered && io.MouseWheel != 0.0f) {
                const float next = std::clamp(scale * std::pow(1.15f, io.MouseWheel), std::min(m_FitScale, 1.0f), 8.0f);
                // Keep the framebuffer point under the cursor where it is.
                const ImVec2 centre(containerStart.x + avail.x * 0.5f, containerStart.y + avail.y * 0.5f);
                const ImVec2 rel(io.MousePos.x - centre.x - m_Pan.x, io.MousePos.y - centre.y - m_Pan.y);
                m_Pan.x -= rel.x * (next / scale - 1.0f);
                m_Pan.y -= rel.y * (next / scale - 1.0f);
                m_Zoom = std::abs(next - m_FitScale) <= 0.01f * m_FitScale ? 0.0f : next;
                if (m_Zoom == 0.0f) m_Pan = ImVec2(0.0f, 0.0f);
            }
            if (m_ViewHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) m_Panning = true;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) m_Panning = false;
            if (m_Panning) { m_Pan.x += io.MouseDelta.x; m_Pan.y += io.MouseDelta.y; }
        } else {
            m_Panning = false;
        }

        // #54 — used to sample the rendered image's luminance behind the top-left and bottom
        // overlays (throttled ~10 Hz, eased) and steer them between light-on-dark and
        // dark-on-light. Fixed opaque plates + fixed light text are legible over anything.

        // In-panel play: first click inside the running view captures mouse/keyboard for the
        // game. (Esc releases — handled in main.cpp.)
        if (playing && !inputEngaged) {
            if (m_ViewHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
                m_EngageClickPending = true;
            }
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
            char buf[320];
            snprintf(buf, sizeof(buf), "%d FPS (%.2f ms)\n%d draw calls\n%d tris / %d verts\n"
                     "cam (%.2f, %.2f, %.2f)\nyaw %.1f  pitch %.1f",
                stats->FPS, stats->FrameMs, stats->DrawCalls, stats->Triangles, stats->Vertices,
                stats->CamPos[0], stats->CamPos[1], stats->CamPos[2], stats->CamYaw, stats->CamPitch);
            ImVec2 textSize = ImGui::CalcTextSize(buf);
            // The floating action bar (EditorLayer::DrawViewportActionBar) is centred along the
            // image's top edge; in a Game panel narrower than about twice its width it reached
            // over this plate and covered the end of the FPS line. Drop below it then. Its rect
            // is last frame's, which is where it is drawn again this frame.
            if (ImGuiWindow* bar = ImGui::FindWindowByName("##ViewportActionBar");
                bar && bar->WasActive && !bar->Hidden) {
                const ImRect plate(statsPos, ImVec2(statsPos.x + textSize.x + 12.0f, statsPos.y + textSize.y + 8.0f));
                if (plate.Overlaps(bar->Rect())) statsPos.y = bar->Rect().Max.y + 6.0f;
            }
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
