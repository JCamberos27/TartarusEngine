#include "Tests/EditorShot.h"

#include "App/LaunchOptions.h"
#include "Components.h"
#include "Core/Paths.h"
#include "CurveEditor.h"
#include "Editor/GameModuleBuild.h"
#include "EditorLayer.h"
#include "EditorModuleAPI.h" // EditorConsoleState
#include "HotReloadEditorModule.h" // EditorModuleHost::ConsoleState
#include "EditorPropertyRows.h"
#include "FirstPersonProcedural.h" // RecoilAsset
#include "RecoilAssetEditor.h"
#include "Tests/FrameCapture.h"
#include "Window.h"
#include "World.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h> // finds dock tabs by title

#include <algorithm>
#include <cstring>
#include <iostream>
#include <iterator>

EditorShot::EditorShot(const LaunchOptions& opt)
    : m_Dir(opt.editorShotDir), m_Curves(opt.editorShotCurves),
      m_Views(opt.editorShotIDE ? std::vector<const char*>{"script-ide"} : opt.editorShotCurves ? std::vector<const char*>{"recoil-curves","recoil-targets","recoil-smoothing","recoil-layers","recoil-misc"} : std::vector<const char*>{"overview", "inspector", "enhancers", "bookmarks", "favorites", "console", "game", "settings", "settings-viewport",
                                                   "settings-performance", "project-settings", "project-tags", "project-build",
                                                   "lighting", "animator", "settings-sounds", "sound-board"}) {}

void EditorShot::BeforeUi(EditorLayer& editor, World& world) {
    // --editor-shot: set up the next view a second ahead of its capture (AfterFrame).
    if (m_Dir.empty()) return;
    const int step = m_Frame++ / kFrames;
    if (m_Frame % kFrames == kFrames / 2 && step < (int)std::size(m_Views)) {
        const std::string view = m_Views[step];
        editor.QuietDockSounds(); // the forced tab selections below aren't a person docking or reordering
        if(view=="script-ide")editor.OpenScriptIDE(GameModuleBuild::Available() ? (GameModuleBuild::SourceRoot()/"src/Game/LightFlickerSystem.cpp").u8string() : std::string());
        auto focus = [](const char* title) {
            for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
                if (std::strstr(w->Name, title) && std::strncmp(w->Name, "##", 2) != 0 && !(w->Flags & ImGuiWindowFlags_ChildWindow)) {
                    // A docked window: select its tab too (focus alone can lose to a tab that asks for focus).
                    if (w->DockNode && w->DockNode->TabBar) w->DockNode->TabBar->NextSelectedTabId = w->TabId;
                    ImGui::SetWindowFocus(w->Name);
                }
        };
        if (view == "overview") {
            EditorModuleHost::ConsoleState().Visible = true; // a tab now, in front for "console"
            for (auto [e, n] : world.Registry.view<NameComponent>().each())
                if (n.Name == "Player Spawn") {
                    if (const auto* o = world.Registry.try_get<OrderComponent>(e)) editor.SelectEntityByOrder(world, o->Value);
                    break;
                }
            focus("###Scene");
            focus("###Assets");
        }
        else if (view == "inspector") editor.SetExpandAllComponents(true);
        // Editor Enhancers: the Inspector and Asset Browser in front (their header rows,
        // tabs and trees), then the bookmarks list open, then the favorites overlay.
        else if (view == "enhancers") { editor.SetExpandAllComponents(false); EditorModuleHost::ConsoleState().Visible = false; focus("###Inspector"); focus("###Assets"); }
        else if (view == "bookmarks") editor.ShotOpenInspectorBookmarks();
        else if (view == "favorites") { ImGui::ClosePopupsExceptModals(); editor.ShotSetFavoritesLocked(true); }
        else if (view == "console") { editor.ShotSetFavoritesLocked(false); EditorModuleHost::ConsoleState().Visible = true; focus("###Console"); }
        // The Game tab the way Play brings it forward: focusing the window alone left
        // Scene in front, so this shot showed the Scene view.
        else if (view == "game") editor.RequestGameTabFocus();
        else if (view == "settings") { editor.RequestSceneTabFocus(); editor.OpenPreferences(); }
        else if (view == "settings-viewport") editor.OpenSettingsCategory(false, 1);
        else if (view == "settings-performance") editor.OpenSettingsCategory(false, 4);
        else if (view == "project-settings") editor.OpenProjectSettings();
        else if (view == "project-tags") editor.OpenSettingsCategory(true, 1);
        else if (view == "project-build") editor.OpenSettingsCategory(true, 2);
        else if (view == "lighting") { editor.CloseSettingsWindow(); editor.SetShowLighting(true); }
        else if (view == "animator") {
            editor.SetShowLighting(false);
            editor.OpenAnimatorWindow("assets/Animations/Controllers/fps_body_locomotion.controller");
        }
        else if (view == "settings-sounds") { editor.SetShowAnimator(false); editor.OpenSettingsCategory(false, 7); }
        else if (view == "sound-board") { editor.CloseSettingsWindow(); editor.SetShowSoundBoard(true); focus("###SoundBoard"); }
    }
}

void EditorShot::DrawPreviewWindows() {
    if (!m_Dir.empty() && m_Curves) {
        static RecoilAsset curvePreview;
        static bool loaded = false;
        if (!loaded) {
            RecoilAsset::LoadFile(ProjectPaths::Resolve("assets/Weapons/AKS74U/AKS74U.recoil"), curvePreview);
            loaded = true;
        }
        ImGui::SetNextWindowPos(ImVec2(600, 120), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(500, 680), ImGuiCond_Always);
        ImGui::Begin("Recoil Curve Editor", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("AKS-74U / PROCEDURAL RECOIL");
        const int recoilView=std::clamp((m_Frame-1)/kFrames,0,4);
        if (recoilView==0) {
        ImGui::TextDisabled("Time in seconds / interpolation alpha");
        ImGui::Separator();
        ImGui::TextUnformatted("Pitch");
        CurveEditor::Options curveOptions; curveOptions.TimeMax=curvePreview.Data.Unity.SemiRotCurve.Length();
        CurveEditor::Draw("pitch", curvePreview.Data.Unity.SemiRotCurve.X, ImVec2(0, 150), curveOptions);
        ImGui::TextUnformatted("Yaw");
        CurveEditor::Draw("yaw", curvePreview.Data.Unity.SemiRotCurve.Y, ImVec2(0, 150), curveOptions);
        ImGui::TextUnformatted("Roll");
        CurveEditor::Draw("roll", curvePreview.Data.Unity.SemiRotCurve.Z, ImVec2(0, 150), curveOptions);
        } else {
            bool previewChanged=false; PropertyRows previewRows(150,previewChanged);
            RecoilAssetEditor::Draw(previewRows,curvePreview.Data,600,recoilView-1);
        }
        ImGui::End();
    }
}

void EditorShot::AfterFrame(Window& window, bool& exitApproved) {
    // --editor-shot: the whole window, once each view has settled.
    if (!m_Dir.empty() && m_Frame > 0 && m_Frame % kFrames == 0) {
        const int step = m_Frame / kFrames - 1;
        if (step < (int)std::size(m_Views)) {
            int w = 0, h = 0;
            glfwGetFramebufferSize(window.Handle(), &w, &h);
            std::vector<unsigned char> flipped = FrameCapture::GrabTopDown(0, w, h);
            for (size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
            const std::string out = FrameCapture::WritePngTo(m_Dir, m_Views[step], w, h, flipped);
            std::cout << "[EditorShot] " << out << std::endl;
        }
        if (step + 1 >= (int)std::size(m_Views)) { exitApproved = true; window.SetShouldClose(true); }
    }
}
