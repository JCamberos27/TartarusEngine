#include "Tests/HarnessView.h"

#include "EditorLayer.h"
#include "EditorSettings.h"
#include "FKeyRow.h"
#include "Core/Log.h"

#include <iostream>

namespace HarnessView {

namespace {
// The names the F5 / F6 pills use (FKeyRow.cpp), for the log line.
const char* const kDrawNames[] = {"Shaded", "Wireframe", "Unlit", "Normals", "Shadow Cascades", "Mip / Texel"};
const char* const kLightNames[] = {"Off", "Lighting Only", "Albedo", "Roughness / Metal", "Light Complexity", "GI Only"};
const char* const kOverlayNames[] = {"Off", "Physics", "AI", "Nav", "Hitboxes"};

template <std::size_t N>
std::string NameOf(const char* const (&names)[N], int i) {
    return i >= 0 && i < (int)N ? names[i] : std::to_string(i);
}
} // namespace

Mode Read(int shading, const EditorSettings& es) {
    Mode m;
    m.Shading = shading;
    m.LightView = es.FRowLightDebug;
    m.Overlay = es.FRowOverlay;
    m.PostFxOff = es.PostFxOff;
    m.PlayDebugOverlay = es.PlayDebugOverlay;
    m.PhysicsDebugDraw = es.PhysicsDebugDrawFlags;
    m.Colliders = es.ShowColliders;
    m.BodyDebug = es.ShowBodyDebug;
    return m;
}

bool Lit(const Mode& m) {
    return m.Shading == 0 && m.LightView == FKeyRow::LightOff && m.Overlay == FKeyRow::OverlayOff && !m.PostFxOff &&
           !m.PlayDebugOverlay && m.PhysicsDebugDraw == 0 && !m.Colliders && !m.BodyDebug;
}

std::string Describe(const Mode& m) {
    if (Lit(m)) return "Shaded, lit";
    std::string s;
    const auto add = [&](const std::string& part) { s += (s.empty() ? "" : ", ") + part; };
    if (m.Shading != 0) add("draw mode " + NameOf(kDrawNames, m.Shading));
    if (m.LightView != FKeyRow::LightOff) add("F5 " + NameOf(kLightNames, m.LightView));
    if (m.Overlay != FKeyRow::OverlayOff) add("F6 " + NameOf(kOverlayNames, m.Overlay));
    if (m.PostFxOff) add("post FX off");
    if (m.PlayDebugOverlay) add("play debug overlay");
    if (m.PhysicsDebugDraw) add("physics debug draw");
    if (m.Colliders) add("colliders shown");
    if (m.BodyDebug) add("body debug");
    return s;
}

void Apply(int& shading, EditorSettings& es) {
    shading = 0;
    es.FRowLightDebug = FKeyRow::LightOff;
    es.FRowOverlay = FKeyRow::OverlayOff;
    es.PostFxOff = false;
    es.PlayDebugOverlay = false;
    es.PhysicsDebugDrawFlags = 0;
    es.ShowColliders = false;
    es.ShowBodyDebug = false;
}

Mode Current(const EditorLayer& editor) { return Read(editor.ShadingModeIndex(), EditorSettings::Get()); }

bool Force(EditorLayer& editor, const char* tag) {
    const Mode before = Current(editor);
    int shading = editor.ShadingModeIndex();
    Apply(shading, EditorSettings::Get());
    editor.SetShadingModeIndex(shading);
    const Mode after = Current(editor);
    if (!Lit(after)) {
        Log::Error(std::string("[") + tag + "] view could not be forced to Shaded, lit: " + Describe(after));
        return false;
    }
    std::cout << "[" << tag << "] view: " << Describe(after);
    if (!Lit(before)) std::cout << " (forced; the prefs had " << Describe(before) << ")";
    std::cout << std::endl;
    return true;
}

} // namespace HarnessView
