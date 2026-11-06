#pragma once

#include <glm/glm.hpp>

class Camera;

// Simple fly-camera controls used only while the editor overlay is open. `orbitPivot`, when
// non-null, is the current selection's world-space center (see EditorLayer::GetSelectionCenter)
// — Alt+Left-drag orbits around it instead of the plain free-look that Right-drag still does.
// Returns true on any frame the fly speed was changed by scroll (so the caller can flash the
// on-screen "Fly speed: N" readout — #236 R2). `viewportHeightPx` is the Scene view's height,
// which scales middle-drag pan so the point under the cursor stays under it (#142).
//
// A fly-speed change is saved once scrolling has paused for kFlySpeedSaveDelay rather than on
// every notch (#142: each notch used to be a synchronous settings write).
class EditorCamera {
public:
    bool Update(Camera& cam, float dt, bool allowLook, bool gizmoDragging, const glm::vec3* orbitPivot,
                float viewportHeightPx);
    // Writes a pending fly-speed change now (force) or once it has sat for the save delay.
    void FlushFlySpeedSave(bool force);

private:
    double m_FlySpeedDirtySince = -1.0; // glfwGetTime() of the last unsaved change, or -1
    // How far the camera is from what it is looking at: the selection when there is one, else the last one used.
    float m_FocusDistance = 10.0f;
};
