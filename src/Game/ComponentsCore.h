#pragma once
// Scene basics every entity can carry: transform, name, order, tags, layers, hierarchy, prefab links.
// Part of the component set; Components.h includes every part.

#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include "RotationMath.h"
#include <glm/gtc/quaternion.hpp>

struct TransformComponent {
    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f};

    // Editor presentation only; never serialized. Keeping the last authored/displayed Euler
    // triple lets the Inspector show the representation the user entered instead of flipping to
    // another equivalent decomposition. Runtime orientation always comes from Rotation.
    glm::vec3 EulerHint{0.0f};

    TransformComponent() = default;
    TransformComponent(const glm::vec3& position, const glm::vec3& eulerDegrees,
                       const glm::vec3& scale)
        : Position(position), Scale(scale) { SetRotationEuler(eulerDegrees); }

    glm::vec3 EulerDegrees() const {
        return NearestEquivalentEuler(EulerYXZFromQuaternion(Rotation), EulerHint);
    }
    void SetRotationEuler(const glm::vec3& eulerDegrees) {
        Rotation = QuaternionFromEulerYXZ(eulerDegrees);
        EulerHint = eulerDegrees;
    }
    void SetRotationQuaternion(const glm::quat& rotation) {
        Rotation = NormalizeRotation(rotation);
        EulerHint = NearestEquivalentEuler(EulerYXZFromQuaternion(Rotation), EulerHint);
    }
};

struct NameComponent {
    std::string Name; // empty = display falls back to a positional name in the Hierarchy
};

// Stable per-entity sequence number, assigned once when the entity is created (World's factory
// methods) and preserved through save/load. The Hierarchy and the scene serializer both order
// entities by this instead of trusting EnTT's live creation order, which a snapshot round-trip
// (undo/redo, Play->Stop) would otherwise flatten - it re-creates every entity in file order,
// so lights/empties (written last) sank permanently below every mesh. Legacy scene files with
// no "order" field fall back to file order on load, matching the old behaviour for them.
struct OrderComponent {
    int Value = 0;
};
// The sort key of an entity with no OrderComponent: last, in both the Hierarchy panel and a parent's Children
// (World::SortChildrenByOrder), so the two orders agree and such an entity is never a LOD Group's level 0.
inline constexpr int kOrderMissing = 2147483647; // INT_MAX

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

// Unity's activeInHierarchy, as a tag: an inactive entity is skipped by rendering,
// collision/raycasting, and editor picking, but keeps all its components and stays listed
// (greyed out) in the Hierarchy so it can be switched back on. Deliberately a tag rather than
// a bool field so "active" is the zero-cost default that needs no component at all.
// DERIVED - never set it directly: World::SyncActiveInHierarchy() puts it on every entity
// that has DeactivatedTag itself or on any ancestor, so switching a parent off hides its
// whole subtree (#201). Runtime systems only ever test this one.
struct InactiveTag {};

// Unity's activeSelf == false: the authored "active" checkbox. Set by the editor and the
// scene/prefab loader, saved as "active": false. See InactiveTag for what it causes.
struct DeactivatedTag {};

// Unity's "Static" checkbox: marks geometry that never moves at runtime. Purely declarative
// today (nothing in the engine batches or bakes yet) — it's here so scenes can be authored with
// the distinction already recorded, rather than needing a re-pass once that optimization lands.
struct StaticTag {};

// Unity's layer field, "-lite" (#236 A1): a small integer slot (0..LayerRegistry::kCount-1)
// grouping entities for editor visibility / pick-locking, camera culling, and (later) PhysX
// collision filtering. Slot 0 is "Default"; the component is omitted entirely for it, so an
// entity on the default layer costs nothing — same zero-default pattern as the tags above.
// Human-readable slot names live in project/layers.json (LayerRegistry), not on the component.
struct LayerComponent {
    int Layer = 0;
};

// SceneVis-lite (#236 B), the editor-only counterparts of Unity's Scene-view hand/lock columns.
// Unlike InactiveTag these change nothing about the object itself — it still renders in the
// Game view, still collides, still ticks, still saves:
//   HiddenInSceneTag — not drawn in the editor Scene viewport, and not click-/marquee-pickable
//                      there (a decluttering aid; the object is still "there").
//   SceneLockedTag   — still drawn in the Scene viewport, but not click-/marquee-pickable, so a
//                      finished piece of set dressing can't be grabbed by accident. Hierarchy
//                      selection and the gizmo still work once selected.
struct HiddenInSceneTag {};

struct SceneLockedTag {};

// Editor Enhancers / vHierarchy (docs/EDITOR_ENHANCERS.md): a Hierarchy row's custom look -
// a Font Awesome icon (by name, e.g. "cube"; "" keeps the kind glyph), a colour washed across the
// row, and the separator/header flag. Editor-only like the two tags above: no runtime system
// reads it. It is scene data on purpose (saved per entity as "hierStyle"), so a team sees the
// same organisation, it travels with prefabs, and undo covers it via the scene snapshot.
struct HierarchyStyleComponent {
    enum Fill : std::uint8_t { None = 0, Flat = 1, Gradient = 2 };
    std::string   Icon;              // FA name; "" = the row's normal kind glyph
    std::uint32_t Color = 0;         // IM_COL32-packed RGBA; 0 = no colour
    std::uint8_t  FillMode = Gradient;
    bool          Separator = false; // draw as a section header: hairline + centred caps name, no toggles
    bool IsEmpty() const { return Icon.empty() && Color == 0 && !Separator; }
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

// Marks the ROOT of a live prefab instance (#236 A2, stage 1-2). The scene file stores only a
// stub for this entity — the source path plus the root's own transform / name / active / layer
// (the "transform-only overrides") — and NOT its descendants; on load the subtree is rebuilt
// from SourcePath and those overrides re-applied on top. So edits to the prefab asset propagate
// to every instance on the next load, while each instance keeps its own placement.
// Descendants carry no marker: the serializer recomputes the owned set from this root each save.
// "Unpack Prefab" removes this component, turning the subtree into plain scene entities.
struct PrefabInstanceComponent {
    std::string SourcePath;
    // Runtime-only: set when SourcePath couldn't be opened on load, so the instance is shown as
    // a broken placeholder rather than silently vanishing. Never serialised.
    bool Missing = false;
    // Runtime-only (#236 A2 stage 3 / #302 Part B): every entity of this instance in
    // prefab-local order — index 0 is this root, the rest follow InstantiatePrefab's creation
    // order (which is deterministic: the .prefab's boxes, then models, then empties, each in
    // file order). Populated by InstantiatePrefab; used at save time to pair each live entity
    // with its pristine counterpart in the .prefab file so per-field overrides can be diffed.
    // Never serialised; entt::null for any descendant the user has since deleted.
    std::vector<entt::entity> InstanceEntities;
};
