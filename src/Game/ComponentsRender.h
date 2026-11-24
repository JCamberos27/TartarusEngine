#pragma once
// What draws: meshes, lights, cameras, particles, terrain, grass, probes, GI, decals, view tags.
// Part of the component set; Components.h includes every part.

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>
#include <entt/entt.hpp>

class Model;
struct TerrainGrid; // TerrainComponent (TerrainGrid.h)
struct GrassDensityMap; // GrassAreaComponent (GrassDensityMap.h)
struct ParticleCurveCache;
struct MaterialAsset; // full definition in MaterialAsset.h; shared_ptr<MaterialAsset> is valid here

// What to draw. Every entity — including former "boxes", which now render through the same
// cube-primitive Model + PBR material as everything else instead of a separate flat-shaded mesh.
// Materials: per-submesh MaterialAsset overrides. A null or missing slot falls through to the
// submesh's imported Material. Empty vector (level-geometry boxes) → all imported materials.
struct RenderableComponent {
    std::shared_ptr<Model> ModelRef;
    std::vector<std::shared_ptr<MaterialAsset>> Materials;

    // #163 - Unity's Mesh Renderer lighting flags. Values are serialized by index.
    // On: casts from the light-facing side. TwoSided: both sides (thin/open meshes like planes and
    // leaves that otherwise leak light). ShadowsOnly: invisible, but still casts.
    enum class ShadowCasting { Off = 0, On = 1, TwoSided = 2, ShadowsOnly = 3 };
    ShadowCasting CastShadows = ShadowCasting::On;
    bool ReceiveShadows = true;
};

// Holds the mesh that was on a RenderableComponent when the user removed "Mesh Renderer" from
// the Inspector, so re-adding the component restores the same mesh instead of a default cube.
// Editor-only and NOT serialized: saving the scene with the mesh removed makes that permanent.
struct DetachedMeshComponent {
    std::shared_ptr<Model> ModelRef;
};

// #162 / #203 - Unity's post-processing Volume. A Global volume applies everywhere; a local one
// applies inside its box (Size, in the object's local units, scaled by its transform) and fades
// in over Blend Distance outside it. Each ticked override pulls that setting toward its value by
// the volume's weight; volumes apply in Priority order (higher wins), on top of the scene's own
// Lighting > Post-processing values.
struct PostProcessVolumeComponent {
    bool      Global = false;
    glm::vec3 Size{10.0f};
    float     BlendDistance = 2.0f;
    float     Weight = 1.0f;
    int       Priority = 0;
    bool OverrideExposure = false;    float ExposureEV = 0.0f;
    bool OverrideTemperature = false; float Temperature = 0.0f;
    bool OverrideTint = false;        float Tint = 0.0f;
    bool OverrideContrast = false;    float Contrast = 0.0f;
    bool OverrideSaturation = false;  float Saturation = 0.0f;
    bool OverrideBloom = false;       float BloomIntensity = 0.1f;
};

// A text label in the world (WorldText.h draws it): signage, measurements on a prototype course. Crisp at any
// distance (a signed-distance font atlas), white and faintly glowing by default, faded out between Fade Start
// and Cull Distance and not drawn past it. Fixed text lies in the entity's local XY plane, read from +Z; scale
// the entity to stretch it.
struct WorldTextComponent {
    enum Facing : int { Fixed = 0, Billboard = 1, Upright = 2 }; // Upright: turns about world up only
    enum AlignMode : int { Left = 0, Center = 1, Right = 2 };
    std::string Text = "Text";
    float     Size = 0.25f;                // line height, metres
    glm::vec3 Color{1.0f};
    float     Brightness = 1.5f;           // on-screen brightness at the scene's fixed exposure; > 1 glows (bloom)
    int       Face = Fixed;
    int       Align = Center;
    float     Outline = 0.0f;              // dark rim, 0..0.4 of the glyph edge band
    float     FadeStart = 25.0f;           // metres from the camera
    float     CullDistance = 35.0f;
    bool      OnTop = false;               // drawn through geometry (no depth test)
};

// #163 - Unity's LOD Group. The entity's direct children are its levels, in order: child 0 is
// LOD 0 (full detail), child 1 is LOD 1, and so on (up to 4; any further children are ignored
// by LOD and always drawn). Each level is used while the group's height on screen, as a fraction
// of the view height, is at least that level's threshold; below the last used level's threshold
// the whole group is culled. Evaluated per view by World::ApplyLod.
struct LODGroupComponent {
    float Lod0 = 0.6f, Lod1 = 0.3f, Lod2 = 0.1f, Lod3 = 0.03f; // screen-height thresholds, 0..1
    float Size = 0.0f; // world-space size used for the screen height; 0 = automatic: the largest axis of LOD 0's world bounds (as Unity)
};

// Runtime only, never saved: this renderer belongs to an LOD level the current view isn't
// using (see LODGroupComponent). Draw and shadow passes skip it; unlike InactiveTag it doesn't
// affect physics, scripts or the Hierarchy.
struct LodCulledTag {};

// Runtime only, never saved: set by FirstPersonPresentation on the arms and weapon entities it
// creates for Play, and read by the renderer's view-model sub-pass. Tagged entities are drawn
// last, after a depth clear, projected with FirstPersonControllerComponent::ViewModelFov instead
// of the view's own FOV — the depth clear is what stops world geometry (metres away) from
// clipping hands that sit ~0.3 m from the eye, and the separate, usually narrower FOV is what
// makes a held weapon read as held rather than stretched into the scene.
//
// A caller that leaves RenderFrameContext::ViewModelFov at its default — the editor Scene tab,
// which previews through its own camera — has no sub-pass to run, so tagged entities are drawn
// there as ordinary scene geometry exactly as before.
struct ViewModelTag {};

// Animated but never drawn: no camera pass, no shadow, no SSAO depth, no search tint. The first-person
// arms rig once the body's arms take its hands (FirstPersonBody::ArmsLateUpdate): it keeps posing (its
// hands are the targets, the sights stay camera-locked) but the body's own arms are what is seen and
// what casts the shadow. Runtime only, like ViewModelTag.
struct PoseSourceTag {};

// Split first-person / world poses (FirstPersonBody with Weapon Arms, in Play). The player's own camera
// shows the first-person pose - the arms and gun exactly as the animations have them - while every other
// view (the Scene view, other players) and every shadow shows the world copy: the same animations, the gun
// placed on the body's shoulder and clear of its head, the body's hands on it there. Runtime only.
//   OwnerViewOnlyTag   - drawn only in the player's own camera; no other view, no shadow, no SSAO elsewhere.
//   HiddenFromOwnerTag - the world copy: every view but the player's own camera; casts the shadows.
struct OwnerViewOnlyTag {};

struct HiddenFromOwnerTag {};

// A dynamic light. Point/Spot use the entity's world position; Directional (the sun) ignores
// position and takes its travel direction from the entity's -Z axis (rotate the entity to aim
// it), matching the spot-cone convention. Every kind goes through the same LightBuffer SSBO and
// the same clustered-forward path; the renderer no longer has a hard-coded sun.
struct LightComponent {
    enum class Type { Point, Spot, Directional };
    Type Kind = Type::Point;
    glm::vec3 Color{1.0f, 0.95f, 0.85f};
    float Intensity = 5.0f;
    float Range = 12.0f;             // Point/Spot: distance at which the contribution reaches zero
    float SpotAngleDegrees = 35.0f;  // Spot: half-angle of the cone
    // Directional: angular diameter of the sun disc, in degrees (~0.53 for Earth's sun). Drives
    // soft-shadow penumbra width once CSM lands; ignored by the other kinds.
    float AngularSizeDegrees = 0.53f;
    // 0 = author Color directly (the swatch is live). >0 = Color is driven from this colour
    // temperature in Kelvin (1500-15000 typical); the Inspector shows a Kelvin slider instead of
    // the raw swatch until you hit "Custom RGB", which zeroes this again.
    float ColorTempK = 0.0f;

    // #203 - Unity's Culling Mask: bit N set = objects on layer N (Layer component) receive this
    // light. -1 (all bits) = everything, the default. Shadows are unaffected.
    int CullingMask = -1;

    // Multiplier on how much this light scatters into the scene's volumetric fog (World::
    // VolumetricEnabled): 1 = physically matched to its Intensity, 0 = no shafts or glow from it.
    float VolumetricIntensity = 1.0f;

    // Point/Spot: a box (in the light's parent space) the light stays inside, so a room's light never shows through its
    // walls, floor or ceiling, shadowed or not. The light fades to nothing over BoundsFade metres inside each face, and
    // the cluster cull skips every froxel outside the box (fewer lights per froxel, not more work).
    bool BoundsEnabled = false;
    glm::vec3 BoundsCenter{0.0f};
    glm::vec3 BoundsSize{4.0f, 3.0f, 4.0f}; // full extents along the parent's axes
    float BoundsFade = 0.15f;

    // Per-light shadow tuning. Always present (not a separate ECS component) so every light can be
    // authored with shadow settings recorded even when it isn't currently casting. `Enabled`
    // replaces the old `CastShadows` bool; the legacy `"castShadows"` scene key still loads into
    // it and is still written for one release. Bias/NormalBias/Softness are multipliers on the
    // shader's existing texel-proportional depth bias / normal-offset / PCF-radius terms — all
    // default 1.0, i.e. byte-identical to pre-phase-2 output. NearPlane feeds the perspective
    // near for spot/point depth passes.
    struct ShadowSettings {
        bool  Enabled    = false;   // was LightComponent::CastShadows
        float Bias       = 1.0f;    // x the shader's texel-proportional depth bias
        float NormalBias = 1.0f;    // x the normal-offset term
        float Softness   = 1.0f;    // x the PCF radius
        float NearPlane  = 0.05f;   // perspective near for spot/point depth
    } Shadow;
};

// A game camera placed in the scene. The Game view renders through the first active one of
// these (in creation order) while editing, so you can frame a shot without walking there.
// In Play (#165) it's also the game camera, unless the scene has a First Person Controller.
// Absent == the Game view falls back to the editor camera and shows a "No camera in scene"
// hint (#36 B10).
struct CameraComponent {
    float FovDegrees = 60.0f;
    float NearPlane = 0.1f;
    float FarPlane = 1000.0f;
};

// #177 - a basic Unity-style Particle System: emits camera-facing soft sprites from the entity's
// origin in a cone around its local +Y, fading/shrinking over their lifetime. Simulated every
// frame (edit mode too, so you can tune it without pressing Play); drawn after transparent
// geometry. Everything below `Live` is runtime state, never serialized.
struct ParticleSystemComponent {
    bool  Emitting = true;
    float Rate = 20.0f;           // particles per second
    int   MaxParticles = 500;
    float Lifetime = 2.0f;        // seconds
    float StartSpeed = 3.0f;      // metres per second
    float Spread = 25.0f;         // cone half-angle around local +Y, degrees (0 = a straight jet)
    float StartSize = 0.25f;      // metres
    float EndSize = 0.05f;
    glm::vec3 StartColor{1.0f, 0.75f, 0.3f};
    glm::vec3 EndColor{1.0f, 0.2f, 0.05f};
    float StartAlpha = 1.0f;
    float EndAlpha = 0.0f;
    float Intensity = 1.0f;       // HDR brightness multiplier (above 1 feeds Bloom)
    float GravityModifier = 0.0f; // x project gravity (1 = falls like a rigidbody)
    int   BlendMode = 1;          // 0 Alpha Blended, 1 Additive
    // Modules. Defaults preserve the original continuous point/cone emitter.
    bool Looping = true;
    float Duration = 5.0f, StartDelay = 0.0f;
    int BurstCount = 0;
    float BurstInterval = 0.0f; // zero: once at the start of each cycle
    float RateOverDistance = 0.0f;
    int Shape = 0; // point, cone volume, sphere, box, disc
    float ShapeRadius = 0.5f, ShapeLength = 1.0f;
    glm::vec3 ShapeBox{1.0f};
    bool ShapeSurface = false, LocalSpace = false;
    float LifetimeRandomness = 0.15f, SpeedRandomness = 0.0f, SizeRandomness = 0.0f;
    glm::vec3 VelocityOverLife{0.0f}, Acceleration{0.0f};
    float Drag = 0.0f;
    float RotationMin = 0.0f, RotationMax = 0.0f;
    float AngularVelocityMin = 0.0f, AngularVelocityMax = 0.0f;
    int Alignment = 0; // camera plane, velocity stretch, horizontal
    float VelocityStretch = 0.1f;
    int ShaderMode = 0; // default sprite/soft disc, procedural smoke
    float SmokeDensity = 1.8f, SmokeNoiseScale = 3.0f;
    float SmokeTurbulence = 0.35f, SmokeSoftness = 0.65f, SmokeEvolution = 0.65f;
    // Project-relative sprite texture; used only by the default shader.
    std::string Texture;
    int TextureChannels = 0; // RGBA, red/green/blue/alpha as opacity mask
    float PivotX = 0.0f, PivotY = 0.0f; // normalized sprite anchor; zero is the centre
    int SheetColumns = 1, SheetRows = 1;
    float SheetFPS = 0.0f; // zero: traverse the sheet once over the particle's life
    bool RandomStartFrame = false, BlendFrames = true;
    int Collision = 0; // off, horizontal plane, PhysX world (Play)
    float CollisionPlaneY = 0.0f, CollisionRadius = 0.01f;
    float Bounce = 0.3f, CollisionFriction = 0.1f, CollisionLifeLoss = 0.0f;
    int MaxBounces = 4;
    // JSON curve keys are reflected for undo/prefabs. Their previews open the shared curve editor.
    std::string SizeCurve = "[[0,1,0,0],[1,1,0,0]]";
    std::string AlphaCurve = "[[0,1,0,0],[1,1,0,0]]";
    std::string SpeedCurve = "[[0,1,0,0],[1,1,0,0]]";
    std::string EmissionCurve = "[[0,1,0,0],[1,1,0,0]]";

    // The flame extras: a particle with Length > 0 (and a Texture set) is drawn as a tongue standing
    // on Pos and pointing along Axis, growing from nothing to Length x Width metres over its life
    // (the system's sizes and colours are ignored). Seed (0..1) picks its shape, Glow scales its
    // emission and Alpha its smoky body.
    struct Particle {
        glm::vec3 Pos{0.0f}, Vel{0.0f};
        float Age = 0.0f, Life = 1.0f;
        glm::vec3 Axis{0.0f, 0.0f, -1.0f};
        float Length = 0.0f, Width = 0.0f, Seed = 0.0f, Glow = 1.0f, Alpha = 1.0f;
        float Rotation = 0.0f, Spin = 0.0f, SizeScale = 1.0f;
        float IntensityScale = 1.0f;
        glm::vec3 Tint{1.0f};
        bool Local = false;
        int FirstFrame = 0, Bounces = 0;
        std::string TextureOverride; // shot-owned flame texture
    };
    std::vector<Particle> Live;
    float EmitAccumulator = 0.0f;
    std::uint32_t Rng = 0x9E3779B9u;
    float Clock = 0.0f, DistanceAccumulator = 0.0f;
    bool WasEmitting = false, HavePreviousPosition = false;
    glm::vec3 PreviousPosition{0.0f};
    std::string EditorCurveIdentity; // hidden, persisted identity for curve-window undo
    std::shared_ptr<ParticleCurveCache> Curves;
};

// Grass Area (GrassRenderer): a rectangle of procedural grass blades on the plane at this entity's position, SizeX
// by SizeZ metres along its local X and Z (turns with the entity's yaw; scale is ignored). Nothing is stored per
// blade: every frame the GPU scatters blades on a jittered grid in the chunks the camera sees, thins them with
// distance (widening the survivors so the field stays solid), and draws them lit like any surface.
struct GrassAreaComponent {
    float SizeX = 20.0f;            // metres along local X
    float SizeZ = 20.0f;            // metres along local Z
    float Density = 300.0f;         // blades per square metre up close
    float Height = 0.45f;           // metres, average blade
    float HeightVariation = 0.35f;  // +- share of Height, blade to blade
    float Width = 0.035f;           // metres at the root
    float Bend = 0.35f;             // natural lean, 0 = upright
    glm::vec3 BaseColor{0.045f, 0.085f, 0.03f};
    glm::vec3 TipColor{0.30f, 0.38f, 0.12f};
    glm::vec3 DryColor{0.34f, 0.30f, 0.16f};
    float ColorVariation = 0.25f;   // blade-to-blade brightness spread
    float Patchiness = 0.5f;        // 0 = even lawn; 1 = clumps, bare spots and dry patches
    float WindStrength = 0.5f;
    float WindSpeed = 1.0f;
    float NearDistance = 14.0f;     // full density and curved blades within this
    float DrawDistance = 60.0f;     // gone beyond this (thinned and shortened toward it)
    float EdgeFade = 1.5f;          // metres over which the field thins out at its edges
    int Seed = 1;
    bool ReceiveShadows = true;
    bool FollowTerrain = false;     // root the blades on the scene's Terrain instead of this object's plane
    // Density mask (GrassDensityMap.h): MapResX x MapResZ 8-bit samples over the area, base64; empty = even.
    std::string DensityMap;
    int MapResX = 0, MapResZ = 0;
    // Runtime (never serialized): the decoded mask and the key it was decoded for (GrassRenderer::Describe).
    mutable std::shared_ptr<const GrassDensityMap> MapCache;
    mutable std::uint64_t MapKey = 0;
};

// A height-grid terrain (TerrainGrid.h). The entity is a model whose path is "primitive://terrain#<key>": the
// mesh is built from these fields (TerrainSystem::Sync) and drawn, shadowed, picked and collided with (a Mesh
// collider) like any model; its material slots are the Renderable's. Heights are 16-bit samples between
// HeightMin and HeightMax, base64, row-major along X, written by a tool (tools/scenes/build_forest.py).
// Grass Areas with Follow Terrain root their blades on the scene's (first) terrain.
struct TerrainComponent {
    float SizeX = 100.0f, SizeZ = 100.0f; // metres, centred on the entity
    int ResX = 2, ResZ = 2;               // samples along X and Z
    float HeightMin = 0.0f, HeightMax = 1.0f;
    float UVTile = 4.0f;                  // metres per texture repeat
    std::string Heights;                  // base64 uint16 samples (not drawn in the Inspector)

    // Runtime (never serialized): the decoded grid and the model built from it, and the key they were built for.
    std::shared_ptr<const TerrainGrid> Grid;
    std::shared_ptr<Model> Built;
    std::uint64_t BuiltKey = 0;
};

// PR14 (#333): placed reflection probe. Position comes from TransformComponent; this component
// adds the box volume for parallax correction and a blend priority. The bake captures the sky
// IBL (procedural or HDRI) at this probe's position; the shader corrects reflection vectors
// to the box boundary, giving room-appropriate reflections without per-probe scene captures.
// No probes in a scene → the shader falls back to the global sky IBL path bit-identically.
struct ReflectionProbeComponent {
    glm::vec3 Size{5.0f, 5.0f, 5.0f}; // full extents of the capture volume, in world units
    float Importance{1.0f};            // higher wins 2-probe selection tie-breaks
    // Render the room into this probe's own cubemap (ReflectionProbeCapture) at scene load / when the probe moves,
    // instead of only parallax-correcting the sky. Off = the old sky-only probe.
    bool CaptureScene{true};
};

// Baked diffuse bounce light for a room (GIData.h, GIVolumes.h): an oriented box of Size (local, x the transform's
// scale) holding a grid of probes about Spacing apart. `--bake-gi` renders each probe's surroundings once per layer -
// one per circuit in Circuits, plus the Outside (moon, sun and sky) when Outside is on - into BakeFile. At runtime each
// layer is weighted by whether its circuit's lights are on and added to the ambient light, fading out over Fade metres
// at the box faces; with an Outside layer the volume replaces the sky's ambient inside it.
struct GIVolumeComponent {
    glm::vec3 Size{4.0f, 3.0f, 4.0f};
    float Spacing = 0.5f;    // metres between probes
    float Fade = 0.05f;      // metres over which it fades at the faces (like a light's Bounds Fade: the walls sit just inside)
    glm::vec3 FadeMin{0.0f}; // per face, box-local axes: the fade of the -x / -y / -z face (0 = use Fade)
    glm::vec3 FadeMax{0.0f}; // ... and of the +x / +y / +z face: a wide one where two volumes share one open space, Fade where a wall is
    float Intensity = 1.0f;  // scales the baked light
    std::string Circuits;    // ';'-separated light circuits to bake a layer each for
    bool Outside = false;    // also bake the moon, sun and sky through the windows (always on)
    std::string BakeFile;    // project-relative .gi written by --bake-gi; this volume's data is found by its name
};

// Clustered PBR decal (docs/DECALS.md). Projects a .decal asset along the entity's local -Z
// (into the surface) through a box of Size metres. Per-instance values multiply / override the
// asset's defaults; Seed drives rotation, scale and flip jitter plus the atlas frame when
// Frame is -1, so the same seed always gives the same look.
struct DecalComponent {
    std::string AssetPath;              // project-relative .decal
    glm::vec3 Size{1.0f, 1.0f, 0.25f};  // width, height, projection depth (metres)
    glm::vec3 Tint{1.0f};
    float Opacity = 1.0f;               // multiplies every channel
    // Per-channel opacity multipliers on top of the asset's channel defaults.
    float AlbedoOp = 1.0f, NormalOp = 1.0f, RoughnessOp = 1.0f, MetalOp = 1.0f, AOOp = 1.0f,
          EmissiveOp = 1.0f;
    int SortOrder = 0;                  // higher draws on top
    int LayerMask = -1;                 // receiver object layers (bit per layer)
    bool AffectStatic = true;
    bool AffectDynamic = true;
    int Seed = 0;
    int Frame = -1;                     // atlas frame; -1 = from Seed
    float RotationJitter = 0.0f;        // degrees, +/-
    float ScaleJitter = 0.0f;           // fraction, +/-
    bool RandomFlip = false;
    // Weathering, added to the asset's values and the scene's Decal Grading.
    float Fade = 0.0f, Bleach = 0.0f, Peel = 0.0f, Buff = 0.0f;
    // Substrate (Substrate.h): what shows where the decal's mask says the paint is gone.
    // 0 Auto: the receiving material's substrate, else the asset's (only for an asset that has one);
    // 1 Asset: always the asset's; 2 Custom: the maps and values below; 3 Off: none;
    // 4 Preset: the .substrate named by SubPreset.
    int SubstrateMode = 0;
    std::string SubPreset;              // project-relative .substrate (Preset)
    std::string SubAlbedo, SubNormal, SubRoughness, SubAO; // project-relative textures (Custom)
    float SubTiling = 1.5f;             // metres per repeat
    glm::vec3 SubTint{1.0f};
    float SubStrength = 1.0f;
    float SubRoughMin = 0.0f, SubRoughMax = 1.0f;
    bool SubMirror = false;
    float SubCropX0 = 0.0f, SubCropY0 = 0.0f, SubCropX1 = 1.0f, SubCropY1 = 1.0f;
};

// Scene-level "how abandoned is this place" grade for every decal (first one in the scene wins).
// Age fades and sun-bleaches; Wear peels, buffs and chips. Read every frame by DecalSystem.
struct DecalGradingComponent {
    float Age = 0.0f;  // 0..1
    float Wear = 0.0f; // 0..1
};

// A graffiti piece going on to a wall with a spray can (Decals P4, Game/SprayCan.h): on the entity whose
// DecalComponent shows the piece's baked decal. It goes on stroke by stroke over Seconds; Elapsed is how far
// it has got. Saved with the scene like every component, so a piece half-sprayed when the game state is saved
// carries on from there when it's loaded.
struct SprayPaintComponent {
    std::string PiecePath;  // project-relative .graffiti
    float Seconds = 10.0f;  // how long the whole piece takes to spray
    float Elapsed = 0.0f;   // seconds sprayed so far
    std::string Painter;    // who is spraying it ("Player", an NPC's name): for the AI and the log
};

// The volumetric blood (Game/Combat/BloodFx): on any object in the scene (the first one counts);
// without one Play uses these same defaults. Copied into BloodFx::Settings when Play starts.
struct BloodSettingsComponent {
    bool Enabled = true;
    float Size = 1.0f;            // x every spray's size (and its playback time, by sqrt: gravity stays right)
    int MaxSprays = 24;           // airborne sprays at once; the oldest is dropped past this
    int MaxStains = 1024;         // stains on the world; past this the farthest small one out of view goes
    float StainLifetime = 0.0f;   // seconds a stain stays (it only ever leaves out of view); 0 = for good
    float DrySeconds = 1800.0f;   // fresh and wet to dried dark and matte, counted out of view only (pools 2.5x, cloth 1.5x)
    float Speed = 2.0f;           // x how fast sprays play and blood lands (1 = the packs' cinematic timing)
    bool Pools = true;            // a pool spreads under each corpse
    bool BodySplats = true;       // blood on bodies, ragdolls and props
    bool GearSpatter = true;      // point-blank blood on the player's gun and hands, and their own wounds
    float EnergyScale = 1.0f;     // x every hit's energy: spray reach and speed, mist, whether rounds exit
    bool ImpactPuffs = true;      // the flipbook burst, mist and droplets the frame a round goes in (Knife packs)
    int Gore = 1;                 // 0 off (no sprays or stains), 1 on
};
