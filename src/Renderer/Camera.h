#pragma once
#include <glm/glm.hpp>
#include <limits>

// First-person camera: position + yaw/pitch, derives view matrix and basis vectors.
// #202 - the one place a perspective projection is built. Every degenerate input is corrected
// here rather than at each call site: far == near divides by zero, a non-positive near makes the
// perspective divide meaningless, and a non-finite aspect (a zero-height viewport gives inf, a
// zero-by-zero one gives NaN) poisons the whole matrix. glm::perspective also ASSERTS on a zero
// aspect, so an unguarded call aborts a Debug build outright.
//
// Call this instead of glm::perspective anywhere camera values reach a projection - they can come
// from a hand-edited scene, an older file, a component written at runtime, or a viewport that has
// momentarily collapsed to zero pixels.
glm::mat4 MakePerspective(float fovDegrees, float aspect, float nearPlane, float farPlane);

class Camera {
public:
    glm::vec3 Position{0.0f, 1.7f, 0.0f};
    float Yaw = -90.0f;   // degrees, facing -Z by default
    float Pitch = 0.0f;
    // Degrees of roll about Front() (#165). Only scene Camera entities set this in Play; the
    // player and editor cameras leave it at 0, so their horizon always stays level.
    float Roll = 0.0f;
    float Fov = 75.0f;

    // Editor-only (the player camera never sets this): orthographic/parallel projection
    // instead of perspective — used for the axis-aligned Front/Top/Right/... views and the
    // isometric view. OrthoHalfHeight is the view volume's half-height in world units (the
    // orthographic equivalent of Fov); scroll-wheel zoom adjusts it directly since dollying
    // the camera position has no visual effect under an orthographic projection.
    bool Orthographic = false;
    float OrthoHalfHeight = 8.0f;

    // Clip planes. Defaults match the old hard-coded ProjectionMatrix() arguments; the editor
    // exposes them per-scene for the fly camera (#236 R2), the player camera leaves them alone.
    float NearPlane = 0.05f;
    float FarPlane  = 500.0f;

    glm::vec3 Front() const;
    glm::vec3 Right() const;
    glm::vec3 Up() const;

    void ProcessMouseLook(float dx, float dy, float sensitivity = 0.1f);

    glm::mat4 ViewMatrix() const;
    // nearOverride / farOverride < 0 (the default) use the NearPlane / FarPlane members.
    glm::mat4 ProjectionMatrix(float aspect, float nearOverride = -1.0f, float farOverride = -1.0f) const;

private:
    // Yaw/Pitch are public and frequently set directly (mouse look, editor camera snapping,
    // scene load) rather than through a setter, so the cache is validated by comparing against
    // the angles it was last computed from rather than an explicit dirty flag. Front()/Right()/
    // Up() are each called several times per frame (movement, view matrix, editor gizmos); this
    // turns that into one trig+normalize pass per changed frame instead of one per call, with
    // Right/Up no longer each recomputing Front from scratch on top of that.
    mutable float m_CachedYaw = std::numeric_limits<float>::quiet_NaN();
    mutable float m_CachedPitch = std::numeric_limits<float>::quiet_NaN();
    mutable float m_CachedRoll = std::numeric_limits<float>::quiet_NaN();
    mutable glm::vec3 m_CachedFront{0.0f, 0.0f, -1.0f};
    mutable glm::vec3 m_CachedRight{1.0f, 0.0f, 0.0f};
    mutable glm::vec3 m_CachedUp{0.0f, 1.0f, 0.0f};
    void RefreshBasisIfNeeded() const;
};
