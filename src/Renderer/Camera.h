#pragma once
#include <glm/glm.hpp>
#include <limits>

// First-person camera: position + yaw/pitch, derives view matrix and basis vectors.
class Camera {
public:
    glm::vec3 Position{0.0f, 1.7f, 0.0f};
    float Yaw = -90.0f;   // degrees, facing -Z by default
    float Pitch = 0.0f;
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
    mutable glm::vec3 m_CachedFront{0.0f, 0.0f, -1.0f};
    mutable glm::vec3 m_CachedRight{1.0f, 0.0f, 0.0f};
    mutable glm::vec3 m_CachedUp{0.0f, 1.0f, 0.0f};
    void RefreshBasisIfNeeded() const;
};
