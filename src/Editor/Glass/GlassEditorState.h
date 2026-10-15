#pragma once
// EditorLayer's state for the Glass theme (EditorLayer::m_Glass, EditorLayer_Glass.cpp): the
// frosted backdrop, the glass compositor, the Glass Workspace panels' scratch and the viewport
// notifications.
//
// Created the first frame the Glass theme is active and destroyed when it's switched off, so the
// default Tartarus theme carries none of it. Animation state is not kept here: it lives in
// Glass::Motion() and exists only while something moves (GlassMotion.h).

#include "GlassBackdrop.h"
#include "GlassCompositor.h"

#include <entt/entt.hpp>
#include <imgui.h>

#include <string>
#include <unordered_set>
#include <vector>

struct GlassEditorState {
    // --- Panel placement (first open docks next to the classic panel; EditorLayer_Glass.cpp) ----
    struct Placement {
        bool Done = false;
        int  Frames = 0;
    };
    Placement PlaceObjects, PlaceProperties, PlaceLibrary;

    // --- Frosted HUDs ---------------------------------------------------------------------------
    GlassBackdrop Backdrop;
    unsigned int  BlurTexture = 0; // this frame's blurred Scene image (0 = tint only)
    // The ambient field behind the docked panels: the Scene image blurred much further.
    GlassBackdrop Ambient;

    // --- Glass under every floating window (EditorLayer::EndFrame renders through it) -----------
    Glass::GlassCompositor Compositor;

    // --- Hierarchy ------------------------------------------------------------------------------
    struct Row {
        entt::entity Entity;
        int          Depth;
        bool         HasChildren;
    };
    std::vector<Row> Rows;                   // the flattened, visible tree, rebuilt every frame
    std::unordered_set<entt::entity> Expanded;
    char HierarchyFilter[64] = {};
    float SelectionRowPrev = -1.0f;          // the selection panel's target row last frame
    entt::entity RevealedFor = entt::null;   // the selection whose ancestors were last auto-expanded

    // --- Inspector ------------------------------------------------------------------------------
    entt::entity InspectorEntity = entt::null; // drives the card entrance cascade on a new selection
    char RenameBuffer[128] = {};

    // --- Library (assets) -----------------------------------------------------------------------
    std::string WaveFolder;  // the folder whose tiles last waved in
    int         WaveCount = 0;

    // --- Notifications --------------------------------------------------------------------------
    struct Toast {
        ImGuiID     Id;
        std::string Title;
        std::string Body;
        bool        Error;
        double      Born;      // Glass::Motion().Now() when it arrived
        double      Dismissed; // < 0 while showing; when the exit started
        float       Y;         // its stack position last frame (< 0 = new), for a smooth restack
    };
    std::vector<Toast> Toasts;
    unsigned long long LastLogSeq = 0;
    bool LogPrimed = false;  // first frame skips the backlog: only new errors toast
    unsigned int NextToastId = 1;

    ImVec2 SnapAt = ImVec2(0.0f, 0.0f); // where the last snapped gizmo drag ended (screen)
    bool   SnapShowing = false;

    // The selection HUD: what it showed last frame, so it can fade out after a deselect.
    entt::entity HudEntity = entt::null;
    bool         HudWasShown = false;
};
