#include "App/EditorCamera.h"

#include "Camera.h"
#include "EditorSettings.h"
#include "EditorSound.h"
#include "Input.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kFlySpeedSaveDelay = 0.75;
}

void EditorCamera::FlushFlySpeedSave(bool force) {
    if (m_FlySpeedDirtySince < 0.0) return;
    if (!force && glfwGetTime() - m_FlySpeedDirtySince < kFlySpeedSaveDelay) return;
    EditorSettings::Save();
    m_FlySpeedDirtySince = -1.0;
}

bool EditorCamera::Update(Camera& cam, float dt, bool allowLook, bool gizmoDragging, const glm::vec3* orbitPivot,
                          float viewportHeightPx) {
    bool flySpeedChanged = false;
    auto trimFlySpeed = [&](double notches) {
        float& fs = EditorSettings::Get().SceneCameraFlySpeed;
        const float prevFlySpeed = fs;
        fs = std::clamp(fs * std::pow(1.15f, (float)notches), 0.5f, 200.0f);
        // Pitched by speed (0.85 at 0.5 up to 1.15 at 200); silent against the clamp, where nothing changed.
        if (fs != prevFlySpeed)
            EditorSound::Play(EditorSound::Event::FlySpeed, -1.0f, 0.85f + 0.3f * std::log(fs / 0.5f) / std::log(400.0f));
        m_FlySpeedDirtySince = glfwGetTime();
        flySpeedChanged = true;
    };
    FlushFlySpeedSave(/*force=*/false);

    // Zoom, dolly and pan all scale with how far the camera is from what it's looking at, like
    // Unity's Scene view (#142); fixed steps crawl on a terrain and overshoot a small prop. The
    // distance is to the selection when there is one; otherwise the last one used is kept, so
    // zooming with nothing selected still accelerates and decelerates smoothly.
    constexpr float kMinFocusDistance = 0.05f, kMaxFocusDistance = 100000.0f;
    if (orbitPivot)
        m_FocusDistance = std::clamp(glm::length(cam.Position - *orbitPivot), kMinFocusDistance, kMaxFocusDistance);
    const bool ctrlHeld = Input::IsKeyDown(GLFW_KEY_LEFT_CONTROL) || Input::IsKeyDown(GLFW_KEY_RIGHT_CONTROL);
    // WASD/QE flythrough only while Right-drag is held — matches Unity's convention exactly,
    // and is required now that W/E/R/T also double as gizmo-tool shortcuts (EditorLayer::Draw):
    // there'd be no way to tell "pressing W to fly" from "pressing W to switch tools" otherwise.
    // A/S/D/Q were never claimed by a tool shortcut, but gating the whole block together keeps
    // the behavior simple and predictable instead of only some of WASDQE requiring Right-drag.
    bool altHeld = Input::IsKeyDown(GLFW_KEY_LEFT_ALT) || Input::IsKeyDown(GLFW_KEY_RIGHT_ALT);

    // Right-drag alone flies the camera (WASDQE + mouselook); Alt+Right-drag is claimed below
    // for dolly instead (Unity's own Scene View split of the same two mouse buttons), so this
    // excludes the Alt-held case rather than fighting it for the same drag.
    if (allowLook && !altHeld && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
        // Scroll while flying (RMB held) trims the fly speed, Unity-style — persisted (#236 R2).
        if (double sc = Input::GetScrollDeltaY(); sc != 0.0) trimFlySpeed(sc);
        float speed = EditorSettings::Get().SceneCameraFlySpeed * dt *
                      (Input::IsKeyDown(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
        glm::vec3 move{0.0f};
        if (Input::IsKeyDown(GLFW_KEY_W)) move += cam.Front();
        if (Input::IsKeyDown(GLFW_KEY_S)) move -= cam.Front();
        if (Input::IsKeyDown(GLFW_KEY_D)) move += cam.Right();
        if (Input::IsKeyDown(GLFW_KEY_A)) move -= cam.Right();
        if (Input::IsKeyDown(GLFW_KEY_E)) move += glm::vec3(0, 1, 0);
        if (Input::IsKeyDown(GLFW_KEY_Q)) move -= glm::vec3(0, 1, 0);
        if (glm::length(move) > 0.0001f) {
            cam.Position += glm::normalize(move) * speed;
        }
        cam.ProcessMouseLook((float)Input::GetMouseDeltaX(), (float)Input::GetMouseDeltaY());
    }

    // Alt+Right-drag dollies the camera along its own view direction - Unity's "hold Alt and
    // drag with the right mouse button to move closer to/further from" gesture. Works the same
    // whether or not anything's selected: it just slides the camera forward/back, same as the
    // scroll-wheel zoom below, just driven by a drag instead of notches. In orthographic mode
    // moving the position has no visual effect (same reason scroll-zoom special-cases it below),
    // so this reuses that OrthoHalfHeight path instead of silently doing nothing.
    if (allowLook && altHeld && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
        // GLFW's mouse-Y delta is screen-down-positive, so dragging UP (delta negative) should
        // dolly IN - hence the sign flip here.
        float dollyAmount = -(float)Input::GetMouseDeltaY();
        if (cam.Orthographic) {
            const float kDollyZoomFactor = 0.01f;
            cam.OrthoHalfHeight = std::clamp(cam.OrthoHalfHeight * (1.0f - dollyAmount * kDollyZoomFactor), 0.25f, 250.0f);
        } else {
            // 0.5% of the focus distance per pixel: ~200px of drag covers the whole way in.
            const float step = std::min(dollyAmount * 0.005f * m_FocusDistance,
                                        m_FocusDistance - kMinFocusDistance); // never through the pivot
            cam.Position += cam.Front() * step;
            m_FocusDistance = std::clamp(m_FocusDistance - step, kMinFocusDistance, kMaxFocusDistance);
        }
    }

    // gizmoDragging excludes this branch while ImGuizmo actually has the left button captured
    // for a handle drag (audit #79) — the two would otherwise fight over the same Alt+LMB chord.
    if (allowLook && orbitPivot && altHeld && !gizmoDragging && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT)) {
        // Same look-rotation input as free-look above, but the camera's position is then pinned
        // to a sphere of constant radius around the pivot instead of staying put — the pivot
        // (the selected object) stays centered in view as the camera swings around it.
        float radius = std::max(glm::length(cam.Position - *orbitPivot), 0.05f);
        cam.ProcessMouseLook((float)Input::GetMouseDeltaX(), (float)Input::GetMouseDeltaY());
        cam.Position = *orbitPivot - cam.Front() * radius;
    }

    // Scroll wheel zooms. Dollying the camera position (as perspective does below) has NO
    // visual effect under an orthographic projection, so orthographic mode instead shrinks/grows
    // OrthoHalfHeight — multiplicatively, so the zoom rate scales with how zoomed-in you already
    // are instead of crawling at large scales or blowing past small ones with a fixed step.
    // Ctrl+scroll (no drag needed) also trims the fly speed — Unity's binding — and shows the
    // same on-screen readout (#236 R2).
    if (allowLook && ctrlHeld && !Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
        if (double sc = Input::GetScrollDeltaY(); sc != 0.0) trimFlySpeed(sc);
    }

    // ...but not while flying (scroll-while-RMB) or trimming speed (Ctrl+scroll) — both handled above.
    if (allowLook && !(!altHeld && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) && !ctrlHeld) {
        double scroll = Input::GetScrollDeltaY();
        if (scroll != 0.0) {
            if (cam.Orthographic) {
                const float kScrollZoomFactor = 0.9f; // per wheel notch
                cam.OrthoHalfHeight = std::clamp(
                    cam.OrthoHalfHeight * powf(kScrollZoomFactor, (float)scroll), 0.25f, 250.0f);
            } else {
                // Same 0.9-per-notch rate as orthographic: each notch closes 10% of the distance,
                // so the camera approaches the focus point without ever passing it.
                const float target = std::clamp(m_FocusDistance * powf(0.9f, (float)scroll),
                                                kMinFocusDistance, kMaxFocusDistance);
                cam.Position += cam.Front() * (m_FocusDistance - target);
                m_FocusDistance = target;
            }
        }
    }

    // Middle-mouse-button drag pans the camera parallel to the view plane — same motion as
    // dragging the nav gizmo's pan button, but usable from anywhere by holding the scroll
    // wheel down, matching the Blender/Maya/Unity convention.
    if (allowLook && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_MIDDLE)) {
        // World units per pixel at the focus distance (#142): the visible height there is
        // 2 * d * tan(fov/2) (or 2 * OrthoHalfHeight), spread over the viewport's pixel rows.
        const float visibleHeight = cam.Orthographic
            ? 2.0f * cam.OrthoHalfHeight
            : 2.0f * m_FocusDistance * std::tan(glm::radians(cam.Fov) * 0.5f);
        const float panPerPixel = visibleHeight / std::max(viewportHeightPx, 1.0f);
        float dx = (float)Input::GetMouseDeltaX();
        float dy = (float)Input::GetMouseDeltaY(); // inverted: up is positive
        cam.Position -= (cam.Right() * dx + cam.Up() * dy) * panPerPixel;
    }
    return flySpeedChanged;
}
