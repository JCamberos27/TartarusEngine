#pragma once
// Semantic icon names for the editor's chrome — the top toolbar strip, the Scene viewport's tool
// palette and view-state chips, and their popovers (EditorModuleToolbar.cpp,
// EditorLayer_ToolPalette.cpp, EditorLayer_Toolbar.cpp's DrawGridSnapPopupBody /
// DrawGizmosPopupBody / DrawViewMenuBody). Phase 3 item 4 (audit #5): two Font Awesome glyphs
// were each carrying two unrelated meanings in this exact chrome —
// ICON_FA_UP_DOWN_LEFT_RIGHT for both Translate and "Toggle Gizmos", and ICON_FA_VECTOR_SQUARE
// for both the Rect tool and Orthographic — which this header resolves by giving each concept
// its own name and, where the collision was real, its own glyph. Orthographic also used to be
// ICON_FA_VECTOR_SQUARE on the toolbar/chip but a different icon (ICON_FA_BORDER_ALL) in the View
// menu for the identical concept; this header is now the one place both read from.
//
// Everywhere else in the editor (Console, Inspector, Hierarchy, Asset Browser) keeps using
// ICON_FA_* directly — this file's scope matches Phase 3's own ("top bar, viewport chrome, tool
// palette"), not a whole-editor icon system with zero real call sites outside it yet.
//
// Correction (review C5, Appendix B #49): every glyph below already resolves to a real ICON_FA_*
// name in the bundled fa-solid-900.ttf — none of the audit mockups' raw emoji (eye/lock/checkbox/
// link/pushpin/etc.) made it into shipped code, so there was never an actual tofu risk here.
// AssertEditorIconsUnique (called once at startup from EditorLayer.cpp, right after the icon font
// merges into the atlas) still checks it mechanically rather than by inspection: every glyph must
// decode to a codepoint inside the range the atlas actually bakes (ICON_MIN_FA..ICON_MAX_16_FA),
// and no two DIFFERENT names may resolve to the same codepoint.
//
// #define, not constexpr const char*: every call site concatenates the icon straight onto a label
// string literal ("ICON_FA_X \"  Label\""), which only works with adjacent STRING LITERALS — a
// const char* variable can't participate in that, only another literal (or, like these, a macro
// that expands to one). Matches ICON_FA_*'s own convention (IconsFontAwesome6.h) for exactly this
// reason.

#include <IconsFontAwesome6.h>
#include <cstdio>
#include <cstddef>

// --- Tool palette (Scene viewport's left rail) ---------------------------------------------------
#define EDITOR_ICON_HAND_TOOL       ICON_FA_HAND
#define EDITOR_ICON_TRANSLATE       ICON_FA_UP_DOWN_LEFT_RIGHT
#define EDITOR_ICON_ROTATE          ICON_FA_ARROWS_SPIN
#define EDITOR_ICON_SCALE           ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER
#define EDITOR_ICON_RECT_TOOL       ICON_FA_VECTOR_SQUARE
#define EDITOR_ICON_UNIVERSAL       ICON_FA_ARROWS_TO_CIRCLE
#define EDITOR_ICON_MEASURE         ICON_FA_RULER
#define EDITOR_ICON_DUPLICATE_ARRAY ICON_FA_CLONE
#define EDITOR_ICON_LOCAL_SPACE     ICON_FA_ARROWS_TO_DOT
#define EDITOR_ICON_WORLD_SPACE     ICON_FA_GLOBE
#define EDITOR_ICON_PIVOT_CENTER    ICON_FA_CIRCLE_DOT
#define EDITOR_ICON_PIVOT_ORIGIN    ICON_FA_CROSSHAIRS

// --- View-state chips (Scene viewport's top-right) ------------------------------------------------
// Each of Perspective/Orthographic now gets its own glyph, and each draw mode gets one no longer
// shared with an unrelated concept (EDITOR_ICON_UNLIT_MODE used to be ICON_FA_SUN, which
// Hierarchy/Gizmos already use for a Directional Light's icon).
#define EDITOR_ICON_PERSPECTIVE   ICON_FA_CUBE
#define EDITOR_ICON_ORTHOGRAPHIC  ICON_FA_BORDER_ALL
#define EDITOR_ICON_SHADED_MODE    ICON_FA_CIRCLE_HALF_STROKE
#define EDITOR_ICON_WIREFRAME_MODE ICON_FA_BORDER_NONE
#define EDITOR_ICON_UNLIT_MODE     ICON_FA_MOON
#define EDITOR_ICON_NORMALS_MODE   ICON_FA_MOUNTAIN
#define EDITOR_ICON_CASCADES_MODE  ICON_FA_LAYER_GROUP
#define EDITOR_ICON_MIP_MODE       ICON_FA_IMAGE

// --- Toggle Gizmos (master viewport-gizmo visibility) ---------------------------------------------
// Was ICON_FA_UP_DOWN_LEFT_RIGHT — identical to EDITOR_ICON_TRANSLATE above despite being a
// completely unrelated action (a visibility toggle, not a transform mode).
#define EDITOR_ICON_TOGGLE_GIZMOS ICON_FA_SHAPES

// --- Grid / Snap -----------------------------------------------------------------------------------
#define EDITOR_ICON_GRID           ICON_FA_TABLE_CELLS
#define EDITOR_ICON_SNAP_TO_GRID   ICON_FA_MAGNET
#define EDITOR_ICON_SNAP_TO_GROUND ICON_FA_DOWN_LONG

// --- Zone A / document strip -------------------------------------------------------------------------
#define EDITOR_ICON_UNDO ICON_FA_ROTATE_LEFT
#define EDITOR_ICON_REDO ICON_FA_ROTATE_RIGHT
#define EDITOR_ICON_SAVE ICON_FA_FLOPPY_DISK

// --- Panel toggles / capture -------------------------------------------------------------------------
#define EDITOR_ICON_STATISTICS ICON_FA_CHART_SIMPLE
#define EDITOR_ICON_CONSOLE    ICON_FA_TERMINAL
#define EDITOR_ICON_HISTORY    ICON_FA_CLOCK_ROTATE_LEFT
#define EDITOR_ICON_CAPTURE    ICON_FA_CAMERA_RETRO
#define EDITOR_ICON_CARET_DOWN ICON_FA_CARET_DOWN

// --- Window controls ---------------------------------------------------------------------------------
#define EDITOR_ICON_WINDOW_MINIMIZE ICON_FA_MINUS
#define EDITOR_ICON_WINDOW_MAXIMIZE ICON_FA_EXPAND
#define EDITOR_ICON_WINDOW_RESTORE  ICON_FA_COMPRESS
#define EDITOR_ICON_WINDOW_CLOSE    ICON_FA_XMARK
// ICON_FA_XMARK also marks "remove this tag" (EditorLayer.cpp) and "delete this layout preset"
// (EditorLayer_Toolbar.cpp), left as plain ICON_FA_XMARK rather than routed through this header:
// all three are the same single concept (dismiss/remove/close), not two different actions
// wearing one glyph, so giving them separate semantic names would manufacture a distinction that
// doesn't exist. Only EDITOR_ICON_WINDOW_CLOSE is registered below for the uniqueness check.

namespace EditorIcons {

struct IconEntry { const char* name; const char* glyph; };
inline constexpr IconEntry kAllIcons[] = {
    {"HandTool", EDITOR_ICON_HAND_TOOL}, {"Translate", EDITOR_ICON_TRANSLATE},
    {"Rotate", EDITOR_ICON_ROTATE}, {"Scale", EDITOR_ICON_SCALE},
    {"RectTool", EDITOR_ICON_RECT_TOOL}, {"Universal", EDITOR_ICON_UNIVERSAL},
    {"Measure", EDITOR_ICON_MEASURE}, {"DuplicateArray", EDITOR_ICON_DUPLICATE_ARRAY},
    {"LocalSpace", EDITOR_ICON_LOCAL_SPACE}, {"WorldSpace", EDITOR_ICON_WORLD_SPACE},
    {"PivotCenter", EDITOR_ICON_PIVOT_CENTER}, {"PivotOrigin", EDITOR_ICON_PIVOT_ORIGIN},
    {"Perspective", EDITOR_ICON_PERSPECTIVE}, {"Orthographic", EDITOR_ICON_ORTHOGRAPHIC},
    {"ShadedMode", EDITOR_ICON_SHADED_MODE}, {"WireframeMode", EDITOR_ICON_WIREFRAME_MODE},
    {"UnlitMode", EDITOR_ICON_UNLIT_MODE}, {"NormalsMode", EDITOR_ICON_NORMALS_MODE},
    {"CascadesMode", EDITOR_ICON_CASCADES_MODE}, {"MipMode", EDITOR_ICON_MIP_MODE},
    {"ToggleGizmos", EDITOR_ICON_TOGGLE_GIZMOS},
    {"Grid", EDITOR_ICON_GRID}, {"SnapToGrid", EDITOR_ICON_SNAP_TO_GRID},
    {"SnapToGround", EDITOR_ICON_SNAP_TO_GROUND},
    {"Undo", EDITOR_ICON_UNDO}, {"Redo", EDITOR_ICON_REDO}, {"Save", EDITOR_ICON_SAVE},
    {"Statistics", EDITOR_ICON_STATISTICS}, {"Console", EDITOR_ICON_CONSOLE},
    {"History", EDITOR_ICON_HISTORY}, {"Capture", EDITOR_ICON_CAPTURE},
    {"CaretDown", EDITOR_ICON_CARET_DOWN},
    {"WindowMinimize", EDITOR_ICON_WINDOW_MINIMIZE}, {"WindowMaximize", EDITOR_ICON_WINDOW_MAXIMIZE},
    {"WindowRestore", EDITOR_ICON_WINDOW_RESTORE}, {"WindowClose", EDITOR_ICON_WINDOW_CLOSE},
};
inline constexpr std::size_t kAllIconsCount = sizeof(kAllIcons) / sizeof(kAllIcons[0]);

// Decodes the first UTF-8 codepoint of a Font Awesome glyph string. Every glyph in this header is
// a single codepoint in Font Awesome's private-use range, 3 bytes (U+E000-U+FFFF), so a 2-byte or
// 4-byte lead is already a sign something's wrong — reported as an invalid (0xFFFFFFFF) codepoint
// rather than mis-decoded, so the range check below still flags it.
inline unsigned DecodeFirstCodepoint(const char* s) {
    if (!s || !s[0]) return 0;
    const unsigned char b0 = (unsigned char)s[0];
    if (b0 < 0x80) return b0;
    if ((b0 & 0xE0) == 0xC0 && s[1]) return ((b0 & 0x1Fu) << 6) | ((unsigned char)s[1] & 0x3Fu);
    if ((b0 & 0xF0) == 0xE0 && s[1] && s[2])
        return ((b0 & 0x0Fu) << 12) | (((unsigned char)s[1] & 0x3Fu) << 6) | ((unsigned char)s[2] & 0x3Fu);
    return 0xFFFFFFFFu;
}

// Called once at startup (EditorLayer.cpp, right after the icon font merges into the atlas). Logs
// rather than asserts/crashes — same reasoning as EditorUIPrimitives::AssertContrastFloor: a
// failed icon should ship visible-but-flagged, not take the editor down. `warnFn` matches that
// function's ContrastWarnFn shape so both share the same startup-logging idiom.
using IconWarnFn = void (*)(const char* message);
inline void AssertEditorIconsUnique(IconWarnFn warnFn) {
    for (std::size_t i = 0; i < kAllIconsCount; ++i) {
        const unsigned cp = DecodeFirstCodepoint(kAllIcons[i].glyph);
        if (cp < ICON_MIN_FA || cp > ICON_MAX_16_FA) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "EditorIcons: '%s' resolves to U+%04X, outside the merged icon range "
                "(U+%04X..U+%04X) — it will render as tofu.",
                kAllIcons[i].name, cp, ICON_MIN_FA, ICON_MAX_16_FA);
            if (warnFn) warnFn(buf);
        }
        for (std::size_t j = i + 1; j < kAllIconsCount; ++j) {
            if (DecodeFirstCodepoint(kAllIcons[j].glyph) == cp) {
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                    "EditorIcons: '%s' and '%s' share the same glyph (U+%04X).",
                    kAllIcons[i].name, kAllIcons[j].name, cp);
                if (warnFn) warnFn(buf);
            }
        }
    }
}

} // namespace EditorIcons
