#pragma once
#include <string>
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include <entt/entt.hpp>

class Model;

// The ECS component set every placed entity (former level-geometry box, imported model, or
// procedural primitive) is built from. Replacing the old WorldBox/PlacedModel split — see
// World.h for the registry and the free functions (ComposeTransform, systems) that operate on
// these.

struct TransformComponent {
    glm::vec3 Position{0.0f};
    glm::vec3 RotationEuler{0.0f}; // degrees
    glm::vec3 Scale{1.0f};
};

struct NameComponent {
    std::string Name; // empty = display falls back to a positional name in the Hierarchy
};

// What to draw. Every entity — including former "boxes", which now render through the same
// cube-primitive Model + PBR material as everything else instead of a separate flat-shaded mesh.
struct RenderableComponent {
    std::shared_ptr<Model> ModelRef;
};

// Presence of this component means the entity participates in collision/raycasting. The actual
// AABB is derived from the entity's RenderableComponent bounds (or, absent one, from
// TransformComponent Position/Scale as a plain unit box) — axis-aligned, ignoring rotation; see
// World::ColliderWorldBounds. Nothing to keep in sync with the visual size as a result.
struct ColliderComponent {
    bool IsTrigger = false; // reserved for future trigger-volume use; solid (blocking) for now
};

// Optional clip triggered from the editor Inspector; formerly PlacedModel-only, now any entity.
struct AudioSourceComponent {
    std::string SoundPath;
};

// Tag only: which Hierarchy section an entity lists under ("Level Geometry" vs "Models") and
// which JSON array (SceneSerializer's "boxes" vs "models") it round-trips through. Carries no
// data of its own — an entity's actual shape/material comes from its RenderableComponent same
// as any other entity.
struct LevelGeometryTag {};

// Free-text classification, mirroring Unity's Tag field — gameplay code and editor search can
// both filter on it ("t:Enemy" in the Hierarchy search box). Absent component == "Untagged".
struct TagComponent {
    std::string Tag = "Untagged";
};

// Unity's GameObject "active" checkbox as a tag: an inactive entity is skipped by rendering,
// collision/raycasting, and editor picking, but keeps all its components and stays listed
// (greyed out) in the Hierarchy so it can be switched back on. Deliberately a tag rather than
// a bool field so "active" is the zero-cost default that needs no component at all.
struct InactiveTag {};

// Unity's "Static" checkbox: marks geometry that never moves at runtime. Purely declarative
// today (nothing in the engine batches or bakes yet) — it's here so scenes can be authored with
// the distinction already recorded, rather than needing a re-pass once that optimization lands.
struct StaticTag {};

// A dynamic light. The renderer supports one directional sun (fixed, from main.cpp) plus up to
// kMaxPointLights of these — a placed light entity's TransformComponent supplies the position.
struct LightComponent {
    enum class Type { Point, Spot };
    Type Kind = Type::Point;
    glm::vec3 Color{1.0f, 0.95f, 0.85f};
    float Intensity = 5.0f;
    float Range = 12.0f;         // distance at which the light's contribution reaches zero
    float SpotAngleDegrees = 35.0f; // Spot only: half-angle of the cone
};

// Present only on entities that are parented, or that have at least one child — an entity with
// no relationships at all simply lacks this component, so the common (flat) case pays no cost.
// When Parent is not entt::null, this entity's TransformComponent is interpreted as LOCAL space
// (relative to the parent's world transform, via World::ComposeWorldTransform) instead of world
// space; every other system (physics, picking, rendering) that used to read TransformComponent
// directly must go through ComposeWorldTransform once an entity can be parented.
struct HierarchyComponent {
    entt::entity Parent = entt::null;
    std::vector<entt::entity> Children;
};
