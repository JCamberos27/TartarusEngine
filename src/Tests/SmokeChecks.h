#pragma once

class AssetLibrary;
class ClusterGrid;
class EditorLayer;
class World;

// The --smoke-test harness's scene-specific checks, run once on a scene whose file name asks for them (15 frames in,
// from main's smoke loop). Each prints a "[SmokeTest]" line and logs an error on failure, which fails the scene.
namespace SmokeChecks {

// smoke_project_sync*: files added, renamed, moved and deleted outside the editor, applied by SyncProjectChanges.
void ProjectSync(EditorLayer& editor, World& world, AssetLibrary& assets);
// smoke_materials*: every loaded material (and a chrome, a plastic and a see-through one) through the material preview.
void MaterialPreviews(World& world);
// smoke_prefab*: a Prefab Mode round trip - a child added in the prefab reaches both placed instances.
void PrefabMode(EditorLayer& editor, World& world, AssetLibrary& assets);
// smoke_glass*: the opt-in Glass editor theme end to end - switched on (all three Glass panels, the
// frosted viewport HUDs and their compute blur, a toast and a snap ring), then off again, after
// which its animation state must hold no memory. Called every frame of the scene; acts on a few.
void GlassTheme(EditorLayer& editor, World& world, int frame);
// smoke_decals_stress*: the full 4096-decal budget on one wall - two frames of a still view bin identically, every
// cluster matches the CPU copy of the cull test (top-most 256 when clamped), and the clamped clusters are counted.
// Called on two consecutive frames; checks on the second.
void DecalStress(const ClusterGrid& grid, int visible, int frame);

} // namespace SmokeChecks
