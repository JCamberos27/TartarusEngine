#pragma once

class AssetLibrary;
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

} // namespace SmokeChecks
