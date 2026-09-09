#include "ComponentRegistry.h"

#include "Components.h"

#include <IconsFontAwesome6.h>

#include <cstring>

namespace ComponentRegistry {

namespace {
std::vector<RegisteredComponent>& Storage() {
    static std::vector<RegisteredComponent> registry;
    return registry;
}
} // namespace

const std::vector<RegisteredComponent>& All() { return Storage(); }

void Add(RegisteredComponent entry) { Storage().push_back(std::move(entry)); }

void RegisterEngineComponents() {
    using T = ReflectFieldType;

    Register<SpinComponent>({
        "Spin", ICON_FA_ARROWS_SPIN,
        "Spins the object around a local axis while playing (SpinSystem, in TartarusGame.dll).",
        "Scripts",
        {
            { "Axis",  T::Vec3,  TARTARUS_REFLECT_FIELD(SpinComponent, Axis),  0.01f, "Local axis to rotate around." },
            { "Speed", T::Float, TARTARUS_REFLECT_FIELD(SpinComponent, Speed), 1.0f,  "Degrees per second." },
        },
    });

    // Migrated from hand-coded serialization/Inspector code onto reflection (#184). Previously
    // this component had runtime behaviour (TransformControllerSystem, in TartarusGame.dll) but
    // no Inspector section or Add Component entry at all — a rule-2 violation of
    // docs/CONVENTIONS.md, invisible until now because nothing else in the editor referenced it.
    // Runtime-only scratch fields (Initialized, Base*, Elapsed) are deliberately not reflected:
    // Play -> Stop reloads the authored scene snapshot, same as before.
    Register<TransformControllerComponent>({
        "Transform Controller", ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT,
        "Drag-and-drop motion script: rotates, translates and scale-pulses the object while playing.",
        "Scripts",
        {
            { "Script Path", T::String, TARTARUS_REFLECT_FIELD(TransformControllerComponent, ScriptPath), 0.0f,
              "Informational: which .tescript asset this behaviour was dragged from." },
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(TransformControllerComponent, Enabled), 0.0f,
              "Runs the motion while playing when checked." },
            { "Rotation Deg/Sec", T::Vec3, TARTARUS_REFLECT_FIELD(TransformControllerComponent, RotationDegPerSec), 1.0f,
              "Continuous local rotation, degrees per second per axis." },
            { "Translation Units/Sec", T::Vec3, TARTARUS_REFLECT_FIELD(TransformControllerComponent, TranslationUnitsPerSec), 0.1f,
              "Continuous local translation, units per second per axis." },
            { "Scale Pulse Amplitude", T::Float, TARTARUS_REFLECT_FIELD(TransformControllerComponent, ScalePulseAmplitude), 0.01f,
              "Fraction of the authored scale (0.2 = +/-20%).", -2.0f, 2.0f },
            { "Scale Pulse Frequency Hz", T::Float, TARTARUS_REFLECT_FIELD(TransformControllerComponent, ScalePulseFrequencyHz), 0.05f,
              "Scale pulse cycles per second.", 0.0f, 50.0f },
        },
    });

    // Migrated from hand-coded serialization/Inspector code onto reflection (#184) — the third
    // such migration. Unlike Camera/Audio/Collider (all ruled out: they carry multi-select
    // and/or custom widgets the generic path doesn't support), Animator was plain fields only,
    // so this is a pure move with no functional loss besides the sub-heading grouping the old
    // hand-written section had (see the field-naming note on AnimatorComponent in Components.h).
    // Runtime-only scratch fields (Initialized, Base*, BaseColor, Elapsed) are not reflected.
    Register<AnimatorComponent>({
        "Animator", ICON_FA_PERSON_RUNNING,
        "Procedural motion driven every frame in Play mode - continuous spin, orbit around an "
        "axis, vertical bob, and light hue-cycling. All fields are additive and reversible "
        "(turning a rate back to 0 undoes its contribution).",
        "Scripts",
        {
            { "Spin Deg/Sec", T::Vec3, TARTARUS_REFLECT_FIELD(AnimatorComponent, SpinDegPerSec), 1.0f,
              "Continuous local rotation, in degrees/second per axis." },
            { "Orbit Axis", T::Vec3, TARTARUS_REFLECT_FIELD(AnimatorComponent, OrbitAxis), 0.01f,
              "Axis this object revolves around, relative to its base position." },
            { "Orbit Speed", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, OrbitDegPerSec), 0.5f,
              "Revolution rate around the orbit axis, in degrees/second." },
            { "Orbit Radius", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, OrbitRadius), 0.05f,
              "Distance from the base position while orbiting, in world units." },
            { "Bob Amplitude", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, BobAmplitude), 0.01f,
              "Vertical sine offset from the base position, in world units." },
            { "Bob Frequency Hz", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, BobFreqHz), 0.02f,
              "Bob rate in Hz (cycles/second)." },
            { "Color Cycle Hz", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, ColorCycleHzPerSec), 0.01f,
              "Hue revolutions/second for this object's Light color. 0 leaves the color alone. "
              "Has no effect without a Light component." },
        },
    });

    // Migrated from hand-coded serialization/Inspector code onto reflection (#302 Wave 1a). The
    // three clip-plane / FOV fields are plain floats and multi-select already works generically
    // (#235); the one Camera-specific control, the "Align to View" button (it reads the editor
    // camera and writes this entity's TransformComponent), stays editor-side as a registered
    // inspector-extra keyed "Camera" (EditorLayer_Inspector.cpp), since reflection describes data
    // only and the editor camera isn't visible from TartarusGame.dll.
    Register<CameraComponent>({
        "Camera", ICON_FA_VIDEO,
        "The Game view renders through this camera while editing, so you can frame a shot "
        "without walking there. Play mode still uses the first-person controller.",
        "Rendering",
        {
            { "Field of View", T::Float, TARTARUS_REFLECT_FIELD(CameraComponent, FovDegrees), 0.25f,
              "Vertical field of view, in degrees.", 1.0f, 179.0f },
            { "Near", T::Float, TARTARUS_REFLECT_FIELD(CameraComponent, NearPlane), 0.01f,
              "Closest distance the camera renders.", 0.001f, 100.0f },
            { "Far", T::Float, TARTARUS_REFLECT_FIELD(CameraComponent, FarPlane), 1.0f,
              "Farthest distance the camera renders.", 0.1f, 100000.0f },
        },
    });

    // Migrated from hand-coded serialization/Inspector code onto reflection (#302 Wave 2b) — the
    // first component to use every widget hint the reflection layer has (Enum, Color, sliders,
    // log scale, format strings, per-Kind VisibleIf, the "Shadows" TreeNode group). Kind values
    // must stay Point=0 / Spot=1 / Directional=2 to match LightComponent::Type and the VisibleIf
    // gates below. ColorTempK is reflected (so it serializes) but EditorHidden — the Kelvin bar,
    // eyedropper and Look-through / Drop-to-surface buttons are editor-only and live in
    // EditorLayer::DrawReflectedComponentExtra / ...ExtraMulti keyed "Light".
    {
        ReflectComponent m;
        m.Name = "Light"; m.Icon = ICON_FA_LIGHTBULB; m.Category = "Rendering";
        m.Tooltip = "Casts light into the scene from this object's position.";
        const char* kMoonShadows = ICON_FA_MOON "  Shadows";
        m.Fields = {
            { "Type", T::Enum, TARTARUS_REFLECT_FIELD(LightComponent, Kind), 0.0f,
              "Point: all directions. Spot: a cone. Directional: a sun (parallel rays, no position or range)." },
            { "Color", T::Color, TARTARUS_REFLECT_FIELD(LightComponent, Color), 0.0f,
              "The light's colour. The button below drives it from a colour temperature instead." },
            { "ColorTempK", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, ColorTempK) },
            { "Intensity", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Intensity), 0.05f,
              "Brightness multiplier - higher is brighter.", 0.0f, 100.0f },
            { "Range", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Range), 0.1f,
              "Distance (in world units) at which the light's effect fades to zero.", 0.1f, 200.0f },
            { "Spot Angle", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, SpotAngleDegrees), 0.5f,
              "Half-angle of the light cone, in degrees.\nThe cone points along the entity's -Z axis - use Rotation to aim it.",
              1.0f, 89.0f },
            { "Angular Size", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, AngularSizeDegrees), 0.05f,
              "Apparent diameter of the sun disc, in degrees (~0.53 = Earth's sun).\nWider = softer shadows.\n"
              "Aim the sun with the entity's Rotation (it shines along -Z).", 0.1f, 20.0f },
            { "Cast Shadows", T::Bool, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.Enabled), 0.0f,
              "Render a shadow map for this light. Spot: one perspective pass (up to 4). Point: a\n"
              "6-face cube (up to 2). Directional: cascaded maps for the sun." },
            { "Shadow Bias", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.Bias), 0.02f,
              "Multiplier on the shader's depth bias. >1 pushes the shadow off contact\n(fixes acne); <1 pulls it back toward the caster.",
              0.0f, 4.0f },
            { "Shadow Normal Bias", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.NormalBias), 0.02f,
              "Multiplier on the normal-offset term. Widen if edges show light bleed.", 0.0f, 4.0f },
            { "Shadow Softness", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.Softness), 0.02f,
              "Multiplier on the PCF filter radius. Higher = wider, softer penumbra.", 0.0f, 4.0f },
            { "Shadow Near Plane", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.NearPlane), 0.01f,
              "Perspective near distance for this light's depth pass.\nRaise it to reclaim depth precision when the light sits far from what it lights.",
              0.001f, 10.0f },
            { "Shadow Resolution", T::Enum, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.Resolution), 0.0f,
              "Per-light shadow-map size (applies after per-light shadow maps)." },
            { "Shadow Update Mode", T::Enum, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.UpdateMode), 0.0f,
              "Dynamic re-renders every frame; Static bakes once (applies after per-light shadow maps)." },
        };
        auto F = [&](const char* name) -> ReflectField& {
            for (auto& f : m.Fields) if (std::strcmp(f.Name, name) == 0) return f;
            return m.Fields[0];
        };
        F("Type").EnumLabels = "Point\0Spot\0Directional\0"; F("Type").EnumCount = 3;
        // Colour + temperature share one control (RGB swatch OR a Kelvin bar, mutually
        // exclusive), plus an eyedropper — all editor-only, so both fields serialize but the
        // widget lives in DrawReflectedComponentExtra / ...ExtraMulti ("Light").
        F("Color").EditorHidden = true;
        F("ColorTempK").EditorHidden = true;
        F("Intensity").Slider = true; F("Intensity").Logarithmic = true; F("Intensity").Format = "%.2f";
        F("Range").Slider = true; F("Range").Logarithmic = true; F("Range").Format = "%.1f";
        F("Range").VisibleIfField = "Type"; F("Range").VisibleIfValue = 2; F("Range").VisibleIfNot = true;
        F("Spot Angle").Slider = true; F("Spot Angle").Format = "%.0f deg";
        F("Spot Angle").VisibleIfField = "Type"; F("Spot Angle").VisibleIfValue = 1;
        F("Angular Size").Slider = true; F("Angular Size").Format = "%.2f deg";
        F("Angular Size").VisibleIfField = "Type"; F("Angular Size").VisibleIfValue = 2;
        for (const char* g : { "Cast Shadows", "Shadow Bias", "Shadow Normal Bias", "Shadow Softness",
                               "Shadow Near Plane", "Shadow Resolution", "Shadow Update Mode" })
            F(g).Group = kMoonShadows;
        for (const char* s : { "Shadow Bias", "Shadow Normal Bias", "Shadow Softness" }) {
            F(s).Slider = true; F(s).Format = "x%.2f";
        }
        F("Shadow Near Plane").Slider = true; F("Shadow Near Plane").Logarithmic = true; F("Shadow Near Plane").Format = "%.3f";
        for (const char* s : { "Shadow Near Plane", "Shadow Resolution", "Shadow Update Mode" }) {
            F(s).VisibleIfField = "Type"; F(s).VisibleIfValue = 2; F(s).VisibleIfNot = true;
        }
        F("Shadow Resolution").EnumLabels = "Follow global\0" "512\0" "1024\0" "2048\0" "4096\0";
        F("Shadow Resolution").EnumCount = 5;
        F("Shadow Update Mode").EnumLabels = "Dynamic\0" "Static (bake once)\0" "Off\0";
        F("Shadow Update Mode").EnumCount = 3;
        Register<LightComponent>(std::move(m));
    }

    // Migrated from hand-coded serialization/Inspector code onto reflection (#302 Wave 3) — first
    // user of ReflectFieldType::AssetRef (the Clip field picks from AssetLibrary::Sounds()). The
    // "Preview" button is editor-only and lives in DrawReflectedComponentExtra keyed "Audio Source".
    {
        ReflectComponent m;
        m.Name = "Audio Source"; m.Icon = ICON_FA_VOLUME_HIGH; m.Category = "Audio";
        m.Tooltip = "A sound clip that can be played from this object.";
        m.Fields = {
            { "Clip", T::AssetRef, TARTARUS_REFLECT_FIELD(AudioSourceComponent, SoundPath), 0.0f,
              "Which imported sound this object plays.\nImport sounds via File > Import, or the Asset Browser." },
            { "Volume", T::Float, TARTARUS_REFLECT_FIELD(AudioSourceComponent, Volume), 0.01f,
              "Playback volume - 1 is unattenuated.", 0.0f, 1.0f },
            { "Loop", T::Bool, TARTARUS_REFLECT_FIELD(AudioSourceComponent, Loop), 0.0f,
              "Restart the clip automatically when it finishes." },
            { "Play On Start", T::Bool, TARTARUS_REFLECT_FIELD(AudioSourceComponent, PlayOnStart), 0.0f,
              "Plays this clip automatically the instant Play mode is entered." },
        };
        m.Fields[0].AssetKind = ReflectAssetKind::Sound;
        m.Fields[1].Slider = true; m.Fields[1].Format = "%.2f";
        Register<AudioSourceComponent>(std::move(m));
    }

    // #185 PR 4 — Rigidbody. Plain reflected fields; the shape comes from the sibling
    // ColliderComponent (hand-written, PR 2) and PhysicsWorld reads both on Play-enter.
    Register<RigidbodyComponent>({
        "Rigidbody", ICON_FA_WEIGHT_HANGING,
        "Simulates this collider as a dynamic PhysX body while playing - it falls, tumbles and "
        "gets pushed around, and its pose is written back every frame. Needs a Collider for its "
        "shape. Play -> Stop restores the authored pose.",
        "Physics",
        {
            { "Mass", T::Float, TARTARUS_REFLECT_FIELD(RigidbodyComponent, Mass), 0.05f,
              "Body mass in kg. Ignored while Kinematic.", 0.001f, 1000.0f },
            { "Use Gravity", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, UseGravity), 0.0f,
              "When off the body still collides but doesn't fall." },
            { "Is Kinematic", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, IsKinematic), 0.0f,
              "Driven from this object's Transform each frame (e.g. an Animator-moved platform) "
              "instead of by forces - pushes other bodies, isn't pushed back." },
            { "Initial Velocity", T::Vec3, TARTARUS_REFLECT_FIELD(RigidbodyComponent, InitialVelocity), 0.1f,
              "Linear velocity (units/sec) applied once, the moment Play starts." },
            { "Linear Damping", T::Float, TARTARUS_REFLECT_FIELD(RigidbodyComponent, LinearDamping), 0.01f,
              "Per-second bleed of linear velocity. 0 = drifts forever.", 0.0f, 10.0f },
            { "Angular Damping", T::Float, TARTARUS_REFLECT_FIELD(RigidbodyComponent, AngularDamping), 0.01f,
              "Per-second bleed of spin.", 0.0f, 10.0f },
            { "Continuous Collision", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, ContinuousCollision), 0.0f,
              "Swept collision so a fast small body can't tunnel a thin wall (#185). Costs a little." },
        },
    });

    // #315 — Mesh Renderer. Registered so the component-registration guard rail counts it and
    // its Icon/Category live here, but BOTH generic paths are opted out: RenderableComponent
    // holds a shared_ptr<Model> (no reflectable fields), its scene form is the box "color/size"
    // or model "path" + "material" written by dedicated code in SceneSerializer, and its add
    // stashes/restores the mesh via DetachedMeshComponent using the editor-only AssetLibrary.
    // The Inspector section (mesh picker, primitive popup, drag-drop, animation controls, the
    // level-geometry Color row) stays hand-coded in EditorLayer_Inspector.cpp. Full
    // genericisation is a follow-up (needs a Model asset-ref field type + an editor-side
    // add/remove hook); see docs/CONVENTIONS.md.
    {
        ReflectComponent m;
        m.Name = "Mesh Renderer"; m.Icon = ICON_FA_DRAW_POLYGON; m.Category = "Rendering";
        m.Tooltip = "The mesh this object draws, and its material color/texture options.";
        m.GenericSerialize = false;
        m.GenericInspector = false;
        Register<RenderableComponent>(std::move(m));
    }
}

// Runs once, before main(). It only appends to All()'s function-local static, so there is no
// static-init-order dependency.
namespace {
struct AutoRegister {
    AutoRegister() { RegisterEngineComponents(); }
} g_autoRegister;
} // namespace

} // namespace ComponentRegistry
