#include "ComponentRegistryInternal.h"

#include "Components.h"
#include "ReflectAttributes.h" // vInspector attributes (Attr)

#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <random>

namespace ComponentRegistry {

// Particles, lights, audio sources and the listener, LOD groups, post-process volumes, world text, rigidbodies, grass, terrain, reflection probes, GI volumes, decals, spray paint, colliders, joints, the mesh renderer, FX & HUD and blood settings.
void RegisterSceneComponents() {
    using T = ReflectFieldType;

    // #177 - a basic CPU particle emitter (fire, sparks, smoke, dust). Simulated every frame,
    // in the editor as well as Play, so it can be tuned live; the live particles aren't saved.
    {
        static const char* kBlendLabels = "Alpha Blended\0Additive\0";
        ReflectComponent m;
        m.Name = "Particle System"; m.Icon = ICON_FA_FIRE; m.Category = "Effects";
        m.Tooltip = "Modular CPU particles: emission cycles, bursts, shapes, forces, lifetime curves, collision, textured flipbooks and procedural smoke.";
        m.Fields = {
            { "Emitting", T::Bool, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Emitting), 0.0f,
              "Spawn new particles. Turning it off lets the live ones finish." },
            { "Rate", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Rate), 0.5f,
              "Particles spawned per second.", 0.0f, 10000.0f },
            { "Max Particles", T::Int, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, MaxParticles), 1.0f,
              "Upper limit on live particles; emission pauses while at the limit.", 1.0f, 100000.0f },
            { "Lifetime", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Lifetime), 0.01f,
              "How long each particle lives, in seconds (varies by about 15%).", 0.01f, 60.0f },
            { "Start Speed", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, StartSpeed), 0.05f,
              "Launch speed, metres per second.", 0.0f, 200.0f },
            { "Spread", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Spread), 0.5f,
              "Cone half-angle around the object's +Y axis, in degrees. 0 = a straight jet, 180 = every direction.",
              0.0f, 180.0f },
            { "Start Size", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, StartSize), 0.005f,
              "Particle diameter when spawned, in metres.", 0.0f, 50.0f },
            { "End Size", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, EndSize), 0.005f,
              "Particle diameter at the end of its life.", 0.0f, 50.0f },
            { "Start Color", T::Color, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, StartColor), 0.0f,
              "Colour when spawned." },
            { "End Color", T::Color, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, EndColor), 0.0f,
              "Colour at the end of its life." },
            { "Start Alpha", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, StartAlpha), 0.01f,
              "Opacity when spawned.", 0.0f, 1.0f },
            { "End Alpha", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, EndAlpha), 0.01f,
              "Opacity at the end of its life (0 = fades out).", 0.0f, 1.0f },
            { "Intensity", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Intensity), 0.05f,
              "Brightness multiplier. Above 1 glows through Bloom.", 0.0f, 100.0f },
            { "Gravity Modifier", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, GravityModifier), 0.01f,
              "How much project gravity pulls the particles (1 = falls like a rigidbody, negative rises).",
              -10.0f, 10.0f },
            { "Blend Mode", T::Enum, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, BlendMode), 0.0f,
              "Additive: brightens what's behind (fire, sparks, magic). Alpha Blended: covers it (smoke, dust)." },
        };
        m.Fields.back().EnumLabels = kBlendLabels;
        m.Fields.back().EnumCount = 2;
        for(auto& f:m.Fields) {
            const std::string name=f.Name;
            f.Group=(name=="Spread")?"Shape":(name=="Gravity Modifier")?"Motion":
                (name=="Blend Mode" || name=="Intensity")?"Rendering":
                (name=="Lifetime" || name=="Start Speed" || name.find("Size")!=std::string::npos || name.find("Color")!=std::string::npos || name.find("Alpha")!=std::string::npos)?"Lifetime":"Emission";
        }
        { ReflectField f{"Shader",T::Enum,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, ShaderMode),0.0f,"Default draws a sprite or soft disc. Procedural Smoke generates animated clouds without a texture.",0.0f,0.0f}; f.Group="Rendering"; f.EnumLabels="Default\0Procedural Smoke\0"; f.EnumCount=2; m.Fields.insert(m.Fields.begin()+12,f); }
        { ReflectField f{"Smoke Density",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SmokeDensity),0.01f,"Optical density of the procedural puff; particle Alpha still controls its final opacity.",0.0f,10.0f}; f.Group="Rendering"; f.VisibleIfField="Shader"; f.VisibleIfValue=1; m.Fields.push_back(f); }
        { ReflectField f{"Smoke Noise Scale",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SmokeNoiseScale),0.01f,"Detail frequency across each puff. Lower values create broader curls.",0.5f,12.0f}; f.Group="Rendering"; f.VisibleIfField="Shader"; f.VisibleIfValue=1; m.Fields.push_back(f); }
        { ReflectField f{"Smoke Turbulence",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SmokeTurbulence),0.01f,"Distorts the cloud body and silhouette into irregular curls.",0.0f,1.0f}; f.Group="Rendering"; f.VisibleIfField="Shader"; f.VisibleIfValue=1; m.Fields.push_back(f); }
        { ReflectField f{"Smoke Softness",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SmokeSoftness),0.01f,"Width of the puff's soft edge.",0.05f,1.0f}; f.Group="Rendering"; f.VisibleIfField="Shader"; f.VisibleIfValue=1; m.Fields.push_back(f); }
        { ReflectField f{"Smoke Evolution",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SmokeEvolution),0.01f,"Speed at which the smoke curls change. Zero freezes each particle's noise shape.",0.0f,4.0f}; f.Group="Rendering"; f.VisibleIfField="Shader"; f.VisibleIfValue=1; m.Fields.push_back(f); }
        { ReflectField f{"Looping",T::Bool,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Looping),0.0f,"Repeat the emission cycle.",0.0f,0.0f}; f.Group="Emission"; m.Fields.push_back(f); }
        { ReflectField f{"Duration",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Duration),0.01f,"Emission cycle duration in seconds.",0.01f,600.0f}; f.Group="Emission"; m.Fields.push_back(f); }
        { ReflectField f{"Start Delay",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, StartDelay),0.01f,"Delay before emission starts.",0.0f,60.0f}; f.Group="Emission"; m.Fields.push_back(f); }
        { ReflectField f{"Burst Count",T::Int,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, BurstCount),1.0f,"Particles emitted at cycle start.",0.0f,100000.0f}; f.Group="Emission"; m.Fields.push_back(f); }
        { ReflectField f{"Burst Interval",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, BurstInterval),0.01f,"Seconds between bursts. Zero emits once per cycle.",0.0f,60.0f}; f.Group="Emission"; m.Fields.push_back(f); }
        { ReflectField f{"Rate Over Distance",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, RateOverDistance),0.1f,"Particles per metre the emitter travels.",0.0f,10000.0f}; f.Group="Emission"; m.Fields.push_back(f); }
        { ReflectField f{"Shape",T::Enum,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Shape),0.0f,"Emission volume in local space.",0.0f,0.0f}; f.Group="Shape"; f.EnumLabels="Point\0Cone\0Sphere\0Box\0Disc\0"; f.EnumCount=5; m.Fields.push_back(f); }
        { ReflectField f{"Shape Radius",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, ShapeRadius),0.01f,"Cone, sphere or disc radius in metres.",0.0f,100.0f}; f.Group="Shape"; m.Fields.push_back(f); }
        { ReflectField f{"Shape Length",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, ShapeLength),0.01f,"Cone volume length along local +Y.",0.0f,100.0f}; f.Group="Shape"; m.Fields.push_back(f); }
        { ReflectField f{"Shape Box",T::Vec3,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, ShapeBox),0.01f,"Box dimensions in metres.",0.0f,100.0f}; f.Group="Shape"; m.Fields.push_back(f); }
        { ReflectField f{"Shape Surface",T::Bool,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, ShapeSurface),0.0f,"Spawn on the shape surface instead of throughout its volume.",0.0f,0.0f}; f.Group="Shape"; m.Fields.push_back(f); }
        { ReflectField f{"Local Space",T::Bool,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, LocalSpace),0.0f,"New particles move with this object's transform.",0.0f,0.0f}; f.Group="Shape"; m.Fields.push_back(f); }
        { ReflectField f{"Lifetime Randomness",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, LifetimeRandomness),0.01f,"Fractional variation around Lifetime.",0.0f,1.0f}; f.Group="Lifetime"; m.Fields.push_back(f); }
        { ReflectField f{"Speed Randomness",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SpeedRandomness),0.01f,"Fractional variation around Start Speed.",0.0f,1.0f}; f.Group="Lifetime"; m.Fields.push_back(f); }
        { ReflectField f{"Size Randomness",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SizeRandomness),0.01f,"Fractional variation of particle size.",0.0f,1.0f}; f.Group="Lifetime"; m.Fields.push_back(f); }
        { ReflectField f{"Velocity Over Life",T::Vec3,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, VelocityOverLife),0.01f,"Additional local or world velocity, metres per second.",-200.0f,200.0f}; f.Group="Motion"; m.Fields.push_back(f); }
        { ReflectField f{"Acceleration",T::Vec3,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Acceleration),0.01f,"Additional local or world acceleration, metres per second squared.",-200.0f,200.0f}; f.Group="Motion"; m.Fields.push_back(f); }
        { ReflectField f{"Drag",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Drag),0.01f,"Exponential velocity damping per second.",0.0f,100.0f}; f.Group="Motion"; m.Fields.push_back(f); }
        { ReflectField f{"Rotation Min",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, RotationMin),1.0f,"Minimum starting rotation in degrees.",-360.0f,360.0f}; f.Group="Motion"; m.Fields.push_back(f); }
        { ReflectField f{"Rotation Max",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, RotationMax),1.0f,"Maximum starting rotation in degrees.",-360.0f,360.0f}; f.Group="Motion"; m.Fields.push_back(f); }
        { ReflectField f{"Angular Velocity Min",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, AngularVelocityMin),1.0f,"Minimum spin in degrees per second.",-3600.0f,3600.0f}; f.Group="Motion"; m.Fields.push_back(f); }
        { ReflectField f{"Angular Velocity Max",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, AngularVelocityMax),1.0f,"Maximum spin in degrees per second.",-3600.0f,3600.0f}; f.Group="Motion"; m.Fields.push_back(f); }
        { ReflectField f{"Collision",T::Enum,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Collision),0.0f,"Plane works in edit mode; world collision uses PhysX during Play.",0.0f,0.0f}; f.Group="Collision"; f.EnumLabels="Off\0Plane\0World\0"; f.EnumCount=3; m.Fields.push_back(f); }
        { ReflectField f{"Collision Plane Y",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, CollisionPlaneY),0.01f,"World-space plane height.",-10000.0f,10000.0f}; f.Group="Collision"; m.Fields.push_back(f); }
        { ReflectField f{"Collision Radius",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, CollisionRadius),0.001f,"Collision skin above the hit surface.",0.0f,10.0f}; f.Group="Collision"; m.Fields.push_back(f); }
        { ReflectField f{"Bounce",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Bounce),0.01f,"Normal velocity retained after a collision.",0.0f,1.0f}; f.Group="Collision"; m.Fields.push_back(f); }
        { ReflectField f{"Collision Friction",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, CollisionFriction),0.01f,"Fraction of tangential velocity lost on contact.",0.0f,1.0f}; f.Group="Collision"; m.Fields.push_back(f); }
        { ReflectField f{"Collision Life Loss",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, CollisionLifeLoss),0.01f,"Fraction of particle lifetime consumed by each collision.",0.0f,1.0f}; f.Group="Collision"; m.Fields.push_back(f); }
        { ReflectField f{"Max Bounces",T::Int,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, MaxBounces),1.0f,"Retire after this many collisions; zero retires at the first hit.",0.0f,100.0f}; f.Group="Collision"; m.Fields.push_back(f); }
        { ReflectField f{"Texture",T::AssetRef,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Texture),0.0f,"Default shader's sprite or flipbook texture; empty draws a soft disc.",0.0f,0.0f}; f.Group="Rendering"; f.AssetKind=ReflectAssetKind::Texture; m.Fields.push_back(f); }
        { ReflectField f{"Texture Channels",T::Enum,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, TextureChannels),0.0f,"RGBA uses the texture's color and alpha. Mask modes use one channel as opacity with the particle color.",0.0f,0.0f}; f.Group="Rendering"; f.EnumLabels="RGBA\0Red Mask\0Green Mask\0Blue Mask\0Alpha Mask\0"; f.EnumCount=5; m.Fields.push_back(f); }
        { ReflectField f{"Pivot X",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, PivotX),0.01f,"Sprite anchor: -1 is the left edge, 0 the centre, 1 the right edge.",-1.0f,1.0f}; f.Group="Rendering"; m.Fields.push_back(f); }
        { ReflectField f{"Pivot Y",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, PivotY),0.01f,"Sprite anchor: -1 is the bottom edge, 0 the centre, 1 the top edge.",-1.0f,1.0f}; f.Group="Rendering"; m.Fields.push_back(f); }
        { ReflectField f{"Alignment",T::Enum,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Alignment),0.0f,"Billboard stays parallel to the camera plane; stretched follows velocity; horizontal lies in world XZ.",0.0f,0.0f}; f.Group="Rendering"; f.EnumLabels="Billboard\0Velocity\0Horizontal\0"; f.EnumCount=3; m.Fields.push_back(f); }
        { ReflectField f{"Velocity Stretch",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, VelocityStretch),0.01f,"Extra length per metre per second.",0.0f,10.0f}; f.Group="Rendering"; m.Fields.push_back(f); }
        { ReflectField f{"Sheet Columns",T::Int,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SheetColumns),1.0f,"Columns in the sprite sheet.",1.0f,256.0f}; f.Group="Rendering"; m.Fields.push_back(f); }
        { ReflectField f{"Sheet Rows",T::Int,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SheetRows),1.0f,"Rows in the sprite sheet.",1.0f,256.0f}; f.Group="Rendering"; m.Fields.push_back(f); }
        { ReflectField f{"Sheet FPS",T::Float,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SheetFPS),0.1f,"Animation speed; zero traverses the sheet once over lifetime.",0.0f,240.0f}; f.Group="Rendering"; m.Fields.push_back(f); }
        { ReflectField f{"Random Start Frame",T::Bool,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, RandomStartFrame),0.0f,"Start each particle at a random sheet cell.",0.0f,0.0f}; f.Group="Rendering"; m.Fields.push_back(f); }
        { ReflectField f{"Blend Frames",T::Bool,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, BlendFrames),0.0f,"Cross-fade neighbouring sprite sheet frames.",0.0f,0.0f}; f.Group="Rendering"; m.Fields.push_back(f); }
        for(auto& f:m.Fields) {
            const std::string name=f.Name;
            if(name=="Texture" || name=="Texture Channels" || name=="Sheet Columns" || name=="Sheet Rows" || name=="Sheet FPS" || name=="Random Start Frame" || name=="Blend Frames") {
                f.VisibleIfField="Shader";f.VisibleIfValue=0;
            }
        }
        { ReflectField f{"Size Curve",T::String,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SizeCurve),0.0f,"Edited in its separate curve window.",0.0f,0.0f}; f.Group="Lifetime"; f.EditorHidden=true; m.Fields.push_back(f); }
        { ReflectField f{"Alpha Curve",T::String,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, AlphaCurve),0.0f,"Edited in its separate curve window.",0.0f,0.0f}; f.Group="Lifetime"; f.EditorHidden=true; m.Fields.push_back(f); }
        { ReflectField f{"Speed Curve",T::String,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, SpeedCurve),0.0f,"Edited in its separate curve window.",0.0f,0.0f}; f.Group="Lifetime"; f.EditorHidden=true; m.Fields.push_back(f); }
        { ReflectField f{"Emission Curve",T::String,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, EmissionCurve),0.0f,"Edited in its separate curve window.",0.0f,0.0f}; f.Group="Lifetime"; f.EditorHidden=true; m.Fields.push_back(f); }
        { ReflectField f{"Curve Editor ID",T::String,TARTARUS_REFLECT_FIELD(ParticleSystemComponent, EditorCurveIdentity)}; f.EditorHidden=true; m.Fields.push_back(f); }
        // The Inspector folds each run of consecutive same-Group fields into one TreeNode, and the
        // list above interleaves groups (the base fields, then each module's extras), which drew
        // 14 headers with Emission / Lifetime / Shape / Motion / Rendering repeated. Gather each
        // group into one run, in module order, keeping the order inside each group.
        {
            static const char* const kGroupOrder[] = {"Emission", "Shape", "Lifetime", "Motion", "Collision", "Rendering"};
            auto rank = [](const ReflectField& f) {
                for (int i = 0; i < (int)std::size(kGroupOrder); ++i)
                    if (f.Group && std::strcmp(f.Group, kGroupOrder[i]) == 0) return i;
                return (int)std::size(kGroupOrder); // ungrouped (hidden bookkeeping) last
            };
            std::stable_sort(m.Fields.begin(), m.Fields.end(),
                             [&](const ReflectField& a, const ReflectField& b) { return rank(a) < rank(b); });
            // One pointer per group name: the Inspector compares Group by pointer, and identical
            // literals are only merged when the compiler pools strings (/GF).
            for (ReflectField& f : m.Fields)
                if (rank(f) < (int)std::size(kGroupOrder)) f.Group = kGroupOrder[rank(f)];
        }
        Register<ParticleSystemComponent>(std::move(m));
    }

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
              "The light's color. The button below drives it from a color temperature instead." }, // #19
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
            { "Volumetric Intensity", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, VolumetricIntensity), 0.02f,
              "How much this light scatters into the volumetric fog (Lighting > Volumetric Fog):\n"
              "its shafts and the glow around it. 1 = matched to Intensity, 0 = none.", 0.0f, 16.0f },
            { "Bounds", T::Bool, TARTARUS_REFLECT_FIELD(LightComponent, BoundsEnabled), 0.0f,
              "Keep this light inside a box (in the parent's space): nothing outside it is lit,\n"
              "so a room's light never shows through walls, floors or ceilings." },
            { "Bounds Center", T::Vec3, TARTARUS_REFLECT_FIELD(LightComponent, BoundsCenter), 0.05f,
              "The box's centre, in the parent's space." },
            { "Bounds Size", T::Vec3, TARTARUS_REFLECT_FIELD(LightComponent, BoundsSize), 0.05f,
              "The box's full size along the parent's axes." },
            { "Bounds Fade", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, BoundsFade), 0.01f,
              "Metres inside each face over which the light fades to nothing.", 0.0f, 2.0f },
            { "Bounds Fade -", T::Vec3, TARTARUS_REFLECT_FIELD(LightComponent, BoundsFadeNeg), 0.01f,
              "Per-face fade (metres) for the box's -X / -Y / -Z faces.\n< 0 = use Bounds Fade. Widen it where a face ends in open air." },
            { "Bounds Fade +", T::Vec3, TARTARUS_REFLECT_FIELD(LightComponent, BoundsFadePos), 0.01f,
              "Per-face fade (metres) for the box's +X / +Y / +Z faces.\n< 0 = use Bounds Fade. Widen it where a face ends in open air." },
            // #203 - drawn as a layer checklist by DrawReflectedComponentExtra("Light").
            { "Culling Mask", T::Int, TARTARUS_REFLECT_FIELD(LightComponent, CullingMask), 0.0f,
              "Which layers this light illuminates." },
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
        F("Volumetric Intensity").Slider = true; F("Volumetric Intensity").Format = "%.2f";
        for (const char* g : { "Cast Shadows", "Shadow Bias", "Shadow Normal Bias", "Shadow Softness",
                               "Shadow Near Plane" })
            F(g).Group = kMoonShadows;
        for (const char* s : { "Shadow Bias", "Shadow Normal Bias", "Shadow Softness" }) {
            F(s).Slider = true; F(s).Format = "x%.2f";
        }
        F("Shadow Near Plane").Slider = true; F("Shadow Near Plane").Logarithmic = true; F("Shadow Near Plane").Format = "%.3f";
        F("Shadow Near Plane").VisibleIfField = "Type"; F("Shadow Near Plane").VisibleIfValue = 2;
        F("Shadow Near Plane").VisibleIfNot = true;
        F("Culling Mask").EditorHidden = true;
        for (const char* b : { "Bounds", "Bounds Center", "Bounds Size", "Bounds Fade", "Bounds Fade -", "Bounds Fade +" }) {
            F(b).VisibleIfField = "Type"; F(b).VisibleIfValue = 2; F(b).VisibleIfNot = true;
        }
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
            { "Output", T::Enum, TARTARUS_REFLECT_FIELD(AudioSourceComponent, Output), 0.0f,
              "Mixer bus this source plays through. Each bus has its own volume in\n"
              "Project Settings > Audio (on top of the Master volume)." },
            { "3D Sound", T::Bool, TARTARUS_REFLECT_FIELD(AudioSourceComponent, Spatial), 0.0f,
              "On: heard from this object's position - panned left/right and quieter with distance.\n"
              "Off: plain 2D, the same everywhere (music, UI sounds)." },
            { "Volume Rolloff", T::Enum, TARTARUS_REFLECT_FIELD(AudioSourceComponent, Rolloff), 0.0f,
              "How volume falls off with distance.\nLogarithmic: realistic, loud up close, long quiet tail.\n"
              "Linear: fades evenly and is silent at Max Distance." },
            { "Min Distance", T::Float, TARTARUS_REFLECT_FIELD(AudioSourceComponent, MinDistance), 0.05f,
              "Within this many metres the sound plays at full volume.", 0.01f, 10000.0f },
            { "Max Distance", T::Float, TARTARUS_REFLECT_FIELD(AudioSourceComponent, MaxDistance), 0.5f,
              "Beyond this the volume stops dropping (Logarithmic) or is silent (Linear).", 0.02f, 10000.0f },
            { "Doppler Level", T::Float, TARTARUS_REFLECT_FIELD(AudioSourceComponent, DopplerLevel), 0.01f,
              "How much the pitch rises as this source and the listener approach each other and\n"
              "falls as they separate. 0 = off, 1 = physically correct (3D sounds only).", 0.0f, 5.0f },
        };
        m.Fields[0].AssetKind = ReflectAssetKind::Sound;
        m.Fields[1].Slider = true; m.Fields[1].Format = "%.2f";
        m.Fields[4].EnumLabels = "SFX\0Music\0Ambient\0UI\0Voice\0"; // #171 - AudioEngine::Bus order
        m.Fields[4].EnumCount = 5;
        m.Fields[6].EnumLabels = "Logarithmic\0Linear\0";
        m.Fields[6].EnumCount = 2;
        // vInspector: the distance settings only matter for a 3D sound - greyed out otherwise.
        for (const char* n : {"Volume Rolloff", "Min Distance", "Max Distance", "Doppler Level"})
            Attr(m, n).EnableIf("3D Sound", 1);
        Register<AudioSourceComponent>(std::move(m));
    }

    // #171 - Unity's Audio Listener.
    Register<LODGroupComponent>({
        "LOD Group", ICON_FA_LAYER_GROUP,
        "Swaps between simpler versions of an object as it gets smaller on screen.\n"
        "Its children are the levels in order: the first child is LOD 0 (full detail), the second\n"
        "LOD 1, and so on (up to 4). Below the last level's threshold the object isn't drawn.",
        "Rendering",
        {
            { "LOD 0", T::Float, TARTARUS_REFLECT_FIELD(LODGroupComponent, Lod0), 0.005f,
              "The first child is drawn while the object is at least this tall on screen\n"
              "(fraction of the view height, e.g. 0.6 = 60%).", 0.0f, 1.0f },
            { "LOD 1", T::Float, TARTARUS_REFLECT_FIELD(LODGroupComponent, Lod1), 0.005f,
              "The second child is drawn from here down to the LOD 2 threshold.", 0.0f, 1.0f },
            { "LOD 2", T::Float, TARTARUS_REFLECT_FIELD(LODGroupComponent, Lod2), 0.005f,
              "The third child is drawn from here down to the LOD 3 threshold.", 0.0f, 1.0f },
            { "LOD 3", T::Float, TARTARUS_REFLECT_FIELD(LODGroupComponent, Lod3), 0.001f,
              "The fourth child is drawn from here down; smaller than this, nothing is drawn.", 0.0f, 1.0f },
            { "Size", T::Float, TARTARUS_REFLECT_FIELD(LODGroupComponent, Size), 0.05f,
              "World-space size used to work out the height on screen, in metres.\n"
              "0 = automatic (the size of the first child's meshes).", 0.0f, 10000.0f },
        },
    });
    // #162 / #203 - Unity's post-processing Volume.
    {
        ReflectComponent m{
            "Post-process Volume", ICON_FA_WAND_MAGIC_SPARKLES,
            "Changes the look (exposure, colour, bloom) while the camera is\n"
            "inside this box, fading in over Blend Distance - or everywhere, when Global. Tick an\n"
            "override to take that setting over from Lighting > Post-processing.",
            "Rendering",
            {
                { "Global", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Global), 0.0f,
                  "Applies everywhere, not just inside the box." },
                { "Size", T::Vec3, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Size), 0.1f,
                  "Box size in this object's local units (scaled by its Scale)." },
                { "Blend Distance", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, BlendDistance), 0.05f,
                  "Metres outside the box over which the effect fades in. 0 = hard edge.", 0.0f, 1000.0f },
                { "Weight", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Weight), 0.01f,
                  "How strongly the overrides apply at full effect.", 0.0f, 1.0f },
                { "Priority", T::Int, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Priority), 0.1f,
                  "Where volumes overlap, higher priority is applied last and wins." },
                { "Override Exposure", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideExposure), 0.0f, "" },
                { "Exposure", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, ExposureEV), 0.05f,
                  "Exposure compensation in stops.", -10.0f, 10.0f },
                { "Override Temperature", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideTemperature), 0.0f, "" },
                { "Temperature", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Temperature), 0.5f,
                  "White balance: negative = cooler / bluer, positive = warmer.", -100.0f, 100.0f },
                { "Override Tint", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideTint), 0.0f, "" },
                { "Tint", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Tint), 0.5f,
                  "White balance: negative = green, positive = magenta.", -100.0f, 100.0f },
                { "Override Contrast", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideContrast), 0.0f, "" },
                { "Contrast", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Contrast), 0.5f, "", -100.0f, 100.0f },
                { "Override Saturation", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideSaturation), 0.0f, "" },
                { "Saturation", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Saturation), 0.5f,
                  "-100 = greyscale.", -100.0f, 100.0f },
                { "Override Bloom", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideBloom), 0.0f,
                  "Needs bloom enabled in Lighting > Post-processing." },
                { "Bloom Intensity", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, BloomIntensity), 0.005f, "", 0.0f, 2.0f },
            },
        };
        for (auto& f : m.Fields)
            if (f.Type == T::Float && std::strcmp(f.Name, "Blend Distance") != 0) { f.Slider = true; f.Format = "%.2f"; }
        Register<PostProcessVolumeComponent>(std::move(m));
    }
    Register<AudioListenerComponent>({
        "Audio Listener", ICON_FA_HEADPHONES,
        "In Play, 3D sounds are heard from this object (its position and facing) instead of from "
        "the game camera - e.g. put it on the player's head in a third-person game.",
        "Audio",
        {
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(AudioListenerComponent, Enabled), 0.0f,
              "When off, the game camera is the listener again." },
        },
    });

    // A text label in the world - signage and measurements (WorldText.h).
    {
        ReflectComponent m{
            "World Text", ICON_FA_FONT,
            "A label drawn in the world: crisp at any distance, faintly glowing, faded out with distance.\n"
            "Fixed text lies in this object's XY plane, read from its +Z side.",
            "Rendering",
            {
                { "Text", T::String, TARTARUS_REFLECT_FIELD(WorldTextComponent, Text), 0.0f,
                  "What it says. A line break starts a new line; UTF-8, with the degree, times and plus-minus signs." },
                { "Size", T::Float, TARTARUS_REFLECT_FIELD(WorldTextComponent, Size), 0.005f,
                  "Line height in metres (before this object's scale).", 0.001f, 100.0f },
                { "Color", T::Color, TARTARUS_REFLECT_FIELD(WorldTextComponent, Color), 0.0f, "" },
                { "Brightness", T::Float, TARTARUS_REFLECT_FIELD(WorldTextComponent, Brightness), 0.01f,
                  "How bright it reads on screen at the scene's exposure: 1 = plain white,\n"
                  "above 1 glows where bloom is on.", 0.0f, 20.0f },
                { "Facing", T::Enum, TARTARUS_REFLECT_FIELD(WorldTextComponent, Face), 0.0f,
                  "Fixed: lies in this object's plane (a floor marking, a wall sign).\n"
                  "Billboard: always faces the camera. Upright: faces it but stays level." },
                { "Align", T::Enum, TARTARUS_REFLECT_FIELD(WorldTextComponent, Align), 0.0f,
                  "Which end of each line sits on this object's position." },
                { "Outline", T::Float, TARTARUS_REFLECT_FIELD(WorldTextComponent, Outline), 0.005f,
                  "A dark rim so the text reads over bright lines and surfaces.", 0.0f, 0.4f },
                { "Fade Start", T::Float, TARTARUS_REFLECT_FIELD(WorldTextComponent, FadeStart), 0.1f,
                  "Begins to fade this far from the camera (metres).", 0.0f, 10000.0f },
                { "Cull Distance", T::Float, TARTARUS_REFLECT_FIELD(WorldTextComponent, CullDistance), 0.1f,
                  "Gone, and not drawn at all, past this distance.", 0.0f, 10000.0f },
                { "On Top", T::Bool, TARTARUS_REFLECT_FIELD(WorldTextComponent, OnTop), 0.0f,
                  "Drawn through walls and objects." },
            },
        };
        m.Fields[4].EnumLabels = "Fixed\0Billboard\0Upright\0";
        m.Fields[4].EnumCount = 3;
        m.Fields[5].EnumLabels = "Left\0Center\0Right\0";
        m.Fields[5].EnumCount = 3;
        m.Fields[3].Slider = true; m.Fields[3].Format = "%.2f";
        m.Fields[6].Slider = true; m.Fields[6].Format = "%.2f";
        Register<WorldTextComponent>(std::move(m));
    }

    // #185 PR 4 — Rigidbody. Plain reflected fields; the shape comes from the sibling
    // ColliderComponent (hand-written, PR 2) and PhysicsWorld reads both on Play-enter.
    {
        ReflectComponent m;
        m.Name = "Rigidbody"; m.Icon = ICON_FA_WEIGHT_HANGING; m.Category = "Physics";
        m.Tooltip =
            "Simulates this collider as a dynamic PhysX body while playing - it falls, tumbles and "
            "gets pushed around, and its pose is written back every frame. Needs a Collider for its "
            "shape. Play -> Stop restores the authored pose.";
        m.Fields = {
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
            { "Freeze Position X", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezePositionX), 0.0f,
              "Lock linear motion along world X." },
            { "Freeze Position Y", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezePositionY), 0.0f,
              "Lock linear motion along world Y." },
            { "Freeze Position Z", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezePositionZ), 0.0f,
              "Lock linear motion along world Z." },
            { "Freeze Rotation X", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezeRotationX), 0.0f,
              "Lock rotation about world X." },
            { "Freeze Rotation Y", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezeRotationY), 0.0f,
              "Lock rotation about world Y." },
            { "Freeze Rotation Z", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezeRotationZ), 0.0f,
              "Lock rotation about world Z." },
            { "Interpolate", T::Enum, TARTARUS_REFLECT_FIELD(RigidbodyComponent, Interpolation), 0.0f,
              "Physics steps at a fixed rate (60 Hz by default); this smooths motion between steps on\n"
              "faster displays. Interpolate: smooth, one step behind. Extrapolate: predicted from\n"
              "velocity, no lag but can overshoot. None: snaps to each step (judders above 60 FPS)." },
        };
        auto F = [&](const char* name) -> ReflectField& {
            for (auto& f : m.Fields) if (std::strcmp(f.Name, name) == 0) return f;
            return m.Fields[0];
        };
        for (const char* g : { "Freeze Position X", "Freeze Position Y", "Freeze Position Z",
                               "Freeze Rotation X", "Freeze Rotation Y", "Freeze Rotation Z" })
            F(g).Group = "Constraints";
        F("Interpolate").EnumLabels = "None\0Interpolate\0Extrapolate\0"; // #168
        F("Interpolate").EnumCount = 3;
        Register<RigidbodyComponent>(std::move(m));
    }

    // PR14 (#333) — Reflection Probe. Registered with the generic reflection path so the
    // Add Component menu, Inspector, and SceneSerializer all work for free.
    {
        ReflectComponent m;
        m.Name = "Grass Area"; m.Icon = ICON_FA_SEEDLING; m.Category = "Rendering";
        m.Tooltip = "Procedural grass over a rectangle on this object's plane: scattered, culled and thinned on the GPU every frame, swaying in the wind, lit by the scene.";
        using G = GrassAreaComponent;
        m.Fields = {
            { "Size X", T::Float, TARTARUS_REFLECT_FIELD(G, SizeX), 0.1f, "Metres along the object's local X.", 0.5f, 1000.0f },
            { "Size Z", T::Float, TARTARUS_REFLECT_FIELD(G, SizeZ), 0.1f, "Metres along the object's local Z.", 0.5f, 1000.0f },
            { "Density", T::Float, TARTARUS_REFLECT_FIELD(G, Density), 5.0f, "Blades per square metre up close.", 1.0f, 2000.0f },
            { "Height", T::Float, TARTARUS_REFLECT_FIELD(G, Height), 0.01f, "Average blade height, metres.", 0.02f, 3.0f },
            { "Height Variation", T::Float, TARTARUS_REFLECT_FIELD(G, HeightVariation), 0.01f, "Blade-to-blade height spread, +- share of Height.", 0.0f, 0.95f },
            { "Width", T::Float, TARTARUS_REFLECT_FIELD(G, Width), 0.001f, "Blade width at the root, metres.", 0.002f, 0.3f },
            { "Bend", T::Float, TARTARUS_REFLECT_FIELD(G, Bend), 0.01f, "Natural lean of the blades: 0 upright.", 0.0f, 1.0f },
            { "Base Color", T::Color, TARTARUS_REFLECT_FIELD(G, BaseColor), 0.0f, "Colour at the roots." },
            { "Tip Color", T::Color, TARTARUS_REFLECT_FIELD(G, TipColor), 0.0f, "Colour at the tips." },
            { "Dry Color", T::Color, TARTARUS_REFLECT_FIELD(G, DryColor), 0.0f, "Colour of the dry patches (Patchiness)." },
            { "Color Variation", T::Float, TARTARUS_REFLECT_FIELD(G, ColorVariation), 0.01f, "Blade-to-blade brightness spread.", 0.0f, 1.0f },
            { "Patchiness", T::Float, TARTARUS_REFLECT_FIELD(G, Patchiness), 0.01f, "0: an even lawn. 1: clumps, thin spots and dry patches.", 0.0f, 1.0f },
            { "Wind Strength", T::Float, TARTARUS_REFLECT_FIELD(G, WindStrength), 0.01f, "How far gusts push the blades over.", 0.0f, 2.0f },
            { "Wind Speed", T::Float, TARTARUS_REFLECT_FIELD(G, WindSpeed), 0.01f, "How fast gusts roll across the field.", 0.0f, 5.0f },
            { "Near Distance", T::Float, TARTARUS_REFLECT_FIELD(G, NearDistance), 0.5f, "Full density and curved blades within this many metres.", 1.0f, 200.0f },
            { "Draw Distance", T::Float, TARTARUS_REFLECT_FIELD(G, DrawDistance), 0.5f, "No grass beyond this many metres: thinned and shortened toward it.", 2.0f, 500.0f },
            { "Edge Fade", T::Float, TARTARUS_REFLECT_FIELD(G, EdgeFade), 0.05f, "Metres over which the field thins out at its edges.", 0.0f, 20.0f },
            { "Seed", T::Int, TARTARUS_REFLECT_FIELD(G, Seed), 1.0f, "Random seed for the scatter." },
            { "Receive Shadows", T::Bool, TARTARUS_REFLECT_FIELD(G, ReceiveShadows), 0.0f, "Darkened by the sun's and lamps' shadows." },
            { "Follow Terrain", T::Bool, TARTARUS_REFLECT_FIELD(G, FollowTerrain), 0.0f, "Root the blades on the scene's Terrain instead of this object's flat plane." },
            { "Map Res X", T::Int, TARTARUS_REFLECT_FIELD(G, MapResX), 1.0f, "Density mask samples along X (0: no mask).", 0.0f, 8192.0f },
            { "Map Res Z", T::Int, TARTARUS_REFLECT_FIELD(G, MapResZ), 1.0f, "Density mask samples along Z (0: no mask).", 0.0f, 8192.0f },
            { "Density Map", T::String, TARTARUS_REFLECT_FIELD(G, DensityMap), 0.0f, "Base64 8-bit density mask." },
        };
        m.Fields.back().EditorHidden = true;
        Register<GrassAreaComponent>(std::move(m));
    }

    {
        ReflectComponent m;
        m.Name = "Terrain"; m.Icon = ICON_FA_MOUNTAIN; m.Category = "Rendering";
        m.Tooltip = "A height-grid terrain, centred on this object: drawn with this object's material slots and collided\n"
                    "with through a Mesh collider. The heights come from a tool (e.g. tools/scenes/build_forest.py).";
        using G = TerrainComponent;
        m.Fields = {
            { "Size X", T::Float, TARTARUS_REFLECT_FIELD(G, SizeX), 0.5f, "Metres along local X.", 1.0f, 10000.0f },
            { "Size Z", T::Float, TARTARUS_REFLECT_FIELD(G, SizeZ), 0.5f, "Metres along local Z.", 1.0f, 10000.0f },
            { "Res X", T::Int, TARTARUS_REFLECT_FIELD(G, ResX), 1.0f, "Height samples along X.", 2.0f, 4097.0f },
            { "Res Z", T::Int, TARTARUS_REFLECT_FIELD(G, ResZ), 1.0f, "Height samples along Z.", 2.0f, 4097.0f },
            { "Height Min", T::Float, TARTARUS_REFLECT_FIELD(G, HeightMin), 0.01f, "Height of a 0 sample, metres." },
            { "Height Max", T::Float, TARTARUS_REFLECT_FIELD(G, HeightMax), 0.01f, "Height of a 65535 sample, metres." },
            { "UV Tile", T::Float, TARTARUS_REFLECT_FIELD(G, UVTile), 0.05f, "Metres per texture repeat.", 0.05f, 1000.0f },
            { "Heights", T::String, TARTARUS_REFLECT_FIELD(G, Heights), 0.0f, "Base64 16-bit samples." },
        };
        m.Fields.back().EditorHidden = true;
        Register<TerrainComponent>(std::move(m));
    }

    Register<ReflectionProbeComponent>({
        "Reflection Probe", ICON_FA_CIRCLE_HALF_STROKE,
        "Defines a box volume for parallax-corrected environment reflections. "
        "Place inside rooms or near reflective surfaces; the 2 nearest probes "
        "blend per draw call. No probes → global sky IBL fallback.",
        "Rendering",
        {
            { "Size", T::Vec3, TARTARUS_REFLECT_FIELD(ReflectionProbeComponent, Size), 0.1f,
              "Full extents of the box capture volume in world units.\n"
              "The parallax correction clips reflection rays to this box boundary.", 0.0f, 0.0f },
            { "Importance", T::Float, TARTARUS_REFLECT_FIELD(ReflectionProbeComponent, Importance), 0.05f,
              "Blend weight priority. Higher wins 2-probe selection when many probes overlap.", 0.0f, 10.0f },
            { "Capture Scene", T::Bool, TARTARUS_REFLECT_FIELD(ReflectionProbeComponent, CaptureScene), 0.0f,
              "Render the room around the probe into its own cubemap (once at scene load and when the probe moves),\n"
              "so metal and glossy surfaces reflect the room. Off = the sky reflection, parallax-corrected.", 0.0f, 0.0f },
        },
    });

    {
        using G = GIVolumeComponent;
        ReflectComponent m;
        m.Name = "GI Volume"; m.Icon = ICON_FA_CUBES; m.Category = "Rendering";
        m.Tooltip = "Baked bounce light for a room: a box of probes that --bake-gi fills once per light circuit (and the\n"
                    "Outside). Each circuit's bounce follows its switch at runtime, with no re-bake. Size it to the room,\n"
                    "padded a few cm into the walls like a light's Bounds.";
        m.Fields = {
            { "Size", T::Vec3, TARTARUS_REFLECT_FIELD(G, Size), 0.05f, "Full extents of the box, metres (x the scale).", 0.0f, 0.0f },
            { "Spacing", T::Float, TARTARUS_REFLECT_FIELD(G, Spacing), 0.05f, "Metres between probes. Re-bake after changing it.", 0.1f, 4.0f },
            { "Fade", T::Float, TARTARUS_REFLECT_FIELD(G, Fade), 0.01f, "Metres over which the light fades out at the box faces.", 0.0f, 4.0f },
            { "Fade Min", T::Vec3, TARTARUS_REFLECT_FIELD(G, FadeMin), 0.01f,
              "Per face, in the box's own axes: the fade of the -x, -y and -z faces. 0 = use Fade. A wider one blends two volumes\n"
              "that share an open space (a wall between them leaks the neighbour's light through it: keep those at Fade).", 0.0f, 4.0f },
            { "Fade Max", T::Vec3, TARTARUS_REFLECT_FIELD(G, FadeMax), 0.01f, "Per face: the fade of the +x, +y and +z faces. 0 = use Fade.", 0.0f, 4.0f },
            { "Intensity", T::Float, TARTARUS_REFLECT_FIELD(G, Intensity), 0.01f, "Scales the baked light.", 0.0f, 16.0f },
            { "Circuits", T::String, TARTARUS_REFLECT_FIELD(G, Circuits), 0.0f,
              "Light circuits to bake a layer each for, ';'-separated. Each layer shows while its circuit is on." },
            { "Outside", T::Bool, TARTARUS_REFLECT_FIELD(G, Outside), 0.0f,
              "Also bake the moon, sun and sky through the windows (always on). With it, the volume replaces the\n"
              "sky's ambient inside its box.", 0.0f, 0.0f },
            { "Bake File", T::String, TARTARUS_REFLECT_FIELD(G, BakeFile), 0.0f, "The .gi file --bake-gi wrote (project-relative)." },
        };
        Register<GIVolumeComponent>(std::move(m));
    }

    // Clustered decals (docs/DECALS.md). The .decal asset holds the look; these are per-placement
    // overrides. The Inspector's extras (EditorLayer_Decals.cpp) add the preview, atlas-frame grid,
    // Edit Asset and Fit to Surface; the box gizmo edits Size, never the Transform's scale.
    {
        ReflectComponent m;
        m.Name = "Decal"; m.Icon = ICON_FA_STAMP; m.Category = "Rendering";
        m.Tooltip = "Projects a .decal (graffiti, grime, stains, signage, paint) onto whatever surfaces its box\n"
                    "touches, along the object's -Z. Blue +Z points out of the surface. Drag a .decal from the\n"
                    "Asset Browser onto a surface to place one.";
        m.Fields = {
            { "Asset", T::AssetRef, TARTARUS_REFLECT_FIELD(DecalComponent, AssetPath), 0.0f,
              "The .decal to project: textures, which channels it writes, fades and paint response.\n"
              "Edit it by selecting it in the Asset Browser (or Edit Asset below)." },
            { "Size", T::Vec3, TARTARUS_REFLECT_FIELD(DecalComponent, Size), 0.01f,
              "Projection box in metres: width, height and depth along -Z. Depth is how far the\n"
              "decal reaches into and out of the surface; keep it shallow so it doesn't bleed onto\n"
              "nearby walls. The Scale gizmo edits this.", 0.001f, 100.0f },
            { "Tint", T::Color, TARTARUS_REFLECT_FIELD(DecalComponent, Tint), 0.0f,
              "Multiplies the asset's colour. White keeps it as authored." },
            { "Opacity", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, Opacity), 0.01f,
              "Overall strength, multiplying every channel.", 0.0f, 1.0f },
            { "Albedo", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, AlbedoOp), 0.01f,
              "Colour strength, on top of the asset's own albedo opacity.", 0.0f, 1.0f },
            { "Normal", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, NormalOp), 0.01f,
              "Surface-relief strength, on top of the asset's normal opacity.", 0.0f, 1.0f },
            { "Roughness", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, RoughnessOp), 0.01f,
              "How much the decal replaces the surface's roughness (wet stains, glossy paint).", 0.0f, 1.0f },
            { "Metal", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, MetalOp), 0.01f,
              "How much the decal replaces the surface's metalness (chipped paint showing bare steel).", 0.0f, 1.0f },
            { "AO", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, AOOp), 0.01f,
              "How much the decal darkens ambient light in its cavities.", 0.0f, 1.0f },
            { "Emissive", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, EmissiveOp), 0.01f,
              "Glow strength (neon paint, screens). With the asset's UV Paint on it glows only under UV light.", 0.0f, 1.0f },
            { "Sort Order", T::Int, TARTARUS_REFLECT_FIELD(DecalComponent, SortOrder), 0.1f,
              "Overlapping decals draw lowest first; higher sits on top. Ties keep a stable order.", -1000.0f, 1000.0f },
            { "Layer Mask", T::Int, TARTARUS_REFLECT_FIELD(DecalComponent, LayerMask), 0.0f,
              "Which object layers receive it, one bit per layer. -1 = every layer." },
            { "Affect Static", T::Bool, TARTARUS_REFLECT_FIELD(DecalComponent, AffectStatic), 0.0f,
              "Project onto static scenery (walls, floors, props that never move)." },
            { "Affect Dynamic", T::Bool, TARTARUS_REFLECT_FIELD(DecalComponent, AffectDynamic), 0.0f,
              "Project onto moving objects and characters that pass through the box." },
            { "Seed", T::Int, TARTARUS_REFLECT_FIELD(DecalComponent, Seed), 1.0f,
              "Drives the jitter below and, with Frame at -1, which atlas frame shows. The same seed\n"
              "always gives the same look." },
            { "Frame", T::Int, TARTARUS_REFLECT_FIELD(DecalComponent, Frame), 0.1f,
              "Atlas frame to show (left to right, top to bottom). -1 picks one from Seed.", -1.0f, 4095.0f },
            { "Rotation Jitter", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, RotationJitter), 0.5f,
              "Random spin about the projection axis, up to this many degrees either way.", 0.0f, 180.0f },
            { "Scale Jitter", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, ScaleJitter), 0.01f,
              "Random size change, up to this fraction either way (0.2 = 80% to 120%).", 0.0f, 0.9f },
            { "Random Flip", T::Bool, TARTARUS_REFLECT_FIELD(DecalComponent, RandomFlip), 0.0f,
              "Mirror horizontally on half of the seeds. Leave off for text and signage." },
            { "Fade", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, Fade), 0.01f,
              "Washes the colour out toward the surface, added to the asset's and the scene's Decal Grading.", 0.0f, 1.0f },
            { "Bleach", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, Bleach), 0.01f,
              "Sun bleaching: desaturates and lightens.", 0.0f, 1.0f },
            { "Peel", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, Peel), 0.01f,
              "Paint flaking off in patches, showing the surface underneath.", 0.0f, 1.0f },
            { "Buff", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, Buff), 0.01f,
              "Scrubbed-off look: a smeared ghost of the mark that someone tried to clean.", 0.0f, 1.0f },
            { "Substrate", T::Enum, TARTARUS_REFLECT_FIELD(DecalComponent, SubstrateMode), 0.0f,
              "The material that shows where the paint is gone (concrete under peeling paint).\n"
              "Auto: the receiving surface's substrate, else the asset's. Asset: always the asset's.\n"
              "Custom: the maps below. Off: none, just the decal's own colour and relief.\n"
              "Preset: a .substrate file (assets/Decals/Substrates)." },
            { "Substrate Preset", T::String, TARTARUS_REFLECT_FIELD(DecalComponent, SubPreset), 0.0f,
              "The .substrate preset to show (Preset): SB_Wall_Concrete, SB_Floor_Concrete, SB_Dark_Wooden_Planks." },
            { "Substrate Albedo", T::AssetRef, TARTARUS_REFLECT_FIELD(DecalComponent, SubAlbedo), 0.0f,
              "Colour map of the exposed material. Its maps take one slot of the decal texture budget." },
            { "Substrate Normal", T::AssetRef, TARTARUS_REFLECT_FIELD(DecalComponent, SubNormal), 0.0f,
              "Optional normal map (OpenGL, green up)." },
            { "Substrate Roughness", T::AssetRef, TARTARUS_REFLECT_FIELD(DecalComponent, SubRoughness), 0.0f,
              "Optional greyscale roughness map, remapped to Rough Min..Max. Empty = Rough Max everywhere." },
            { "Substrate AO", T::AssetRef, TARTARUS_REFLECT_FIELD(DecalComponent, SubAO), 0.0f,
              "Optional greyscale ambient-occlusion map." },
            { "Substrate Tiling", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, SubTiling), 0.01f,
              "Metres per repeat, laid out in world space so neighbouring decals line up.", 0.05f, 50.0f },
            { "Substrate Tint", T::Color, TARTARUS_REFLECT_FIELD(DecalComponent, SubTint), 0.0f,
              "Multiplies the substrate's colour." },
            { "Substrate Strength", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, SubStrength), 0.01f,
              "How fully the substrate replaces the paint's own look where it shows.", 0.0f, 1.0f },
            { "Substrate Rough Min", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, SubRoughMin), 0.01f,
              "Roughness where the roughness map is black.", 0.0f, 1.0f },
            { "Substrate Rough Max", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, SubRoughMax), 0.01f,
              "Roughness where the roughness map is white (everywhere, without a map).", 0.0f, 1.0f },
            { "Substrate Mirror", T::Bool, TARTARUS_REFLECT_FIELD(DecalComponent, SubMirror), 0.0f,
              "Mirror every other repeat: hides the seams of a sheet that doesn't tile by itself." },
            { "Substrate Crop X0", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, SubCropX0), 0.005f,
              "The part of the image that repeats, left edge (0..1).", 0.0f, 1.0f },
            { "Substrate Crop Y0", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, SubCropY0), 0.005f,
              "Top edge (0..1, down the image).", 0.0f, 1.0f },
            { "Substrate Crop X1", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, SubCropX1), 0.005f,
              "Right edge.", 0.0f, 1.0f },
            { "Substrate Crop Y1", T::Float, TARTARUS_REFLECT_FIELD(DecalComponent, SubCropY1), 0.005f,
              "Bottom edge.", 0.0f, 1.0f },
        };
        auto F = [&](const char* name) -> ReflectField& {
            for (auto& f : m.Fields) if (std::strcmp(f.Name, name) == 0) return f;
            return m.Fields[0];
        };
        F("Asset").AssetKind = ReflectAssetKind::Decal;
        for (const char* s : {"Opacity", "Albedo", "Normal", "Roughness", "Metal", "AO", "Emissive",
                              "Scale Jitter", "Fade", "Bleach", "Peel", "Buff"})
            F(s).Slider = true;
        F("Rotation Jitter").Slider = true;
        F("Rotation Jitter").Format = "%.0f deg";
        for (const char* s : {"Albedo", "Normal", "Roughness", "Metal", "AO", "Emissive"}) F(s).Group = "Channels";
        for (const char* s : {"Sort Order", "Layer Mask", "Affect Static", "Affect Dynamic"}) F(s).Group = "Receivers";
        for (const char* s : {"Seed", "Frame", "Rotation Jitter", "Scale Jitter", "Random Flip"}) F(s).Group = "Variation";
        for (const char* s : {"Fade", "Bleach", "Peel", "Buff"}) F(s).Group = "Weathering";
        F("Substrate").EnumLabels = "Auto\0Asset\0Custom\0Off\0Preset\0";
        F("Substrate").EnumCount = 5;
        for (const char* s : {"Substrate Albedo", "Substrate Normal", "Substrate Roughness", "Substrate AO"})
            F(s).AssetKind = ReflectAssetKind::Texture;
        for (const char* s : {"Substrate Strength", "Substrate Rough Min", "Substrate Rough Max", "Substrate Crop X0",
                              "Substrate Crop Y0", "Substrate Crop X1", "Substrate Crop Y1"})
            F(s).Slider = true;
        for (const char* s : {"Substrate", "Substrate Albedo", "Substrate Normal", "Substrate Roughness", "Substrate AO",
                              "Substrate Tiling", "Substrate Tint", "Substrate Strength", "Substrate Rough Min",
                              "Substrate Rough Max", "Substrate Mirror", "Substrate Crop X0", "Substrate Crop Y0",
                              "Substrate Crop X1", "Substrate Crop Y1"}) {
            F(s).Group = "Substrate";
            if (std::strcmp(s, "Substrate") != 0) {
                F(s).VisibleIfField = "Substrate";
                F(s).VisibleIfValue = 2; // Custom
            }
        }
        F("Substrate Preset").Group = "Substrate";
        F("Substrate Preset").VisibleIfField = "Substrate";
        F("Substrate Preset").VisibleIfValue = 4; // Preset
        m.Buttons.push_back({"Randomize Seed", ICON_FA_DICE,
                             "A new random seed: a different rotation, scale, flip and atlas frame within the jitter set above.",
                             0, [](World&, entt::entity, void* c, int) {
                                 static std::mt19937 rng{std::random_device{}()};
                                 static_cast<DecalComponent*>(c)->Seed = (int)(rng() & 0x7FFFFFFF);
                             }});
        Register<DecalComponent>(std::move(m));
    }

    // Scene-wide decal grade: one slider ages every decal in the level at once.
    {
        ReflectComponent m;
        m.Name = "Decal Grading"; m.Icon = ICON_FA_HOURGLASS_HALF; m.Category = "Rendering";
        m.Tooltip = "How abandoned this place looks: ages and wears every decal in the scene on top of each\n"
                    "decal's own weathering. Add one to any object; the first in the scene is used.";
        m.Fields = {
            { "Age", T::Float, TARTARUS_REFLECT_FIELD(DecalGradingComponent, Age), 0.01f,
              "0 = fresh. Higher fades colours and sun-bleaches them.", 0.0f, 1.0f },
            { "Wear", T::Float, TARTARUS_REFLECT_FIELD(DecalGradingComponent, Wear), 0.01f,
              "0 = intact. Higher peels, scrubs and chips paint and graffiti.", 0.0f, 1.0f },
        };
        for (ReflectField& f : m.Fields) f.Slider = true;
        Register<DecalGradingComponent>(std::move(m));
    }

    // Decals P4: a graffiti piece being sprayed on (SprayCanAction, Game/SprayCan.h). Runtime-made, but an
    // ordinary component so it saves with the game state.
    {
        ReflectComponent m;
        m.Name = "Spray Paint"; m.Icon = ICON_FA_SPRAY_CAN; m.Category = "Rendering";
        m.Tooltip = "A graffiti piece going on to the wall stroke by stroke, as a spray can puts it there. The\n"
                    "Decal on this object shows the piece; this says how far the spraying has got.";
        m.Fields = {
            { "Piece", T::String, TARTARUS_REFLECT_FIELD(SprayPaintComponent, PiecePath), 0.0f,
              "The .graffiti piece (project-relative) being sprayed." },
            { "Seconds", T::Float, TARTARUS_REFLECT_FIELD(SprayPaintComponent, Seconds), 0.1f,
              "How long the whole piece takes to spray.", 0.5f, 600.0f },
            { "Elapsed", T::Float, TARTARUS_REFLECT_FIELD(SprayPaintComponent, Elapsed), 0.1f,
              "Seconds sprayed so far; at Seconds the piece is finished.", 0.0f, 600.0f },
            { "Painter", T::String, TARTARUS_REFLECT_FIELD(SprayPaintComponent, Painter), 0.0f,
              "Who is spraying it (the player or an NPC's name)." },
        };
        Register<SprayPaintComponent>(std::move(m));
    }

    // Phase 4 (#6) item 1 — Collider. Registered for the generic Inspector/Add-Component path
    // only: GenericSerialize stays false because a dedicated hand-written pass in
    // SceneSerializer.cpp already reads/writes it under a "collider" JSON key predating this
    // reflection layer (issue #185 PR 2) — flipping GenericSerialize on would have the generic
    // per-component pass write it a second time under a different key, corrupting saved scenes
    // with no benefit (Inspector genericization works directly against the live struct, entirely
    // independent of how it's persisted). HalfExtents is EditorHidden: it means something
    // different per Shape (three half-extents for Box, just .x as a radius for Sphere, .x/.y as
    // radius/half-height for Capsule, unused for Convex Hull/Mesh) — a single ReflectField can't
    // relabel/reshape itself like that, so the per-shape size row stays hand-coded in
    // DrawReflectedComponentExtra("Collider", Bottom), same escape hatch Light's Kelvin bar uses.
    {
        ReflectComponent m;
        m.Name = "Collider"; m.Icon = ICON_FA_CUBE; m.Category = "Physics";
        static const char* kCombineLabels = "Average\0Minimum\0Multiply\0Maximum\0";
        m.Tooltip = "Blocks movement and is hit by raycasts. While playing this drives a PhysX actor:\n"
                    "static on its own, dynamic/kinematic with a sibling Rigidbody. The shape below\n"
                    "picks its geometry.";
        m.GenericSerialize = false;
        m.Key = "collider"; // matches the hand-written SceneSerializer JSON key (#185 PR 2) — see ReflectComponent::Key
        m.Fields = {
            { "Shape", T::Enum, TARTARUS_REFLECT_FIELD(ColliderComponent, Kind), 0.0f,
              "Box / Sphere / Capsule are analytic primitives sized by the fields below.\n"
              "Convex Hull / Mesh cook a shape from this object's mesh + scale:\n"
              "Convex Hull works on any body; Mesh (triangle mesh) is static / kinematic only\n"
              "(a dynamic Mesh collider falls back to a convex hull)." },
            { "Is Trigger", T::Bool, TARTARUS_REFLECT_FIELD(ColliderComponent, IsTrigger), 0.0f,
              "A trigger reports enter / stay / exit overlaps to gameplay\nbut doesn't block movement." },
            { "Center", T::Vec3, TARTARUS_REFLECT_FIELD(ColliderComponent, Center), 0.05f,
              "Shape offset from the object's origin, local space." },
            { "Bounciness", T::Float, TARTARUS_REFLECT_FIELD(ColliderComponent, Bounciness), 0.0f,
              "Restitution: 0 stops dead, 1 loses no energy on a bounce.", 0.0f, 1.0f },
            { "Friction", T::Float, TARTARUS_REFLECT_FIELD(ColliderComponent, Friction), 0.0f,
              "Dynamic friction: how much a sliding contact is slowed. 0 = ice.", 0.0f, 2.0f },
            { "Static Friction", T::Float, TARTARUS_REFLECT_FIELD(ColliderComponent, StaticFriction), 0.0f,
              "How hard it is to start sliding. Usually equal to or a little above Friction.", 0.0f, 2.0f },
            { "Friction Combine", T::Enum, TARTARUS_REFLECT_FIELD(ColliderComponent, FrictionCombine), 0.0f,
              "How this collider's friction combines with the one it touches.\n"
              "If the two differ, the later mode in the list wins (Maximum beats everything)." },
            { "Bounce Combine", T::Enum, TARTARUS_REFLECT_FIELD(ColliderComponent, BounceCombine), 0.0f,
              "How this collider's bounciness combines with the one it touches (same rule)." },
            { "HalfExtents", T::Vec3, TARTARUS_REFLECT_FIELD(ColliderComponent, HalfExtents), 0.05f },
        };
        auto F = [&](const char* name) -> ReflectField& {
            for (auto& f : m.Fields) if (std::strcmp(f.Name, name) == 0) return f;
            return m.Fields[0];
        };
        F("Shape").EnumLabels = "Box\0Sphere\0Capsule\0Convex Hull\0Mesh\0"; F("Shape").EnumCount = 5;
        F("Bounciness").Slider = true; F("Bounciness").Format = "%.2f";
        F("Friction").Slider = true; F("Friction").Format = "%.2f";
        F("Static Friction").Slider = true; F("Static Friction").Format = "%.2f";
        F("Friction Combine").EnumLabels = kCombineLabels; F("Friction Combine").EnumCount = 4;
        F("Bounce Combine").EnumLabels = kCombineLabels; F("Bounce Combine").EnumCount = 4;
        F("HalfExtents").EditorHidden = true;
        Register<ColliderComponent>(std::move(m));
    }

    // Phase 4 (#6) item 1 — Joint. Same GenericSerialize = false reasoning as Collider (a
    // dedicated hand-written "joint" JSON pass predates this, #185 PR 11). ConnectedOrder needs a
    // custom entity picker (enumerates every OrderComponent in the scene, not a fixed asset/enum
    // list); Axis is only meaningful for Hinge/Slider and UseLimit/LimitLower/LimitUpper only for
    // Hinge/Slider/Distance — VisibleIfField only expresses "equals" or "not-equals" ONE sibling
    // value, not membership in a set of two, so these stay EditorHidden and hand-coded together in
    // DrawReflectedComponentExtra("Joint", Bottom) rather than stretching the reflection schema
    // for one component (see the schema note if a second component ever needs this).
    {
        ReflectComponent m;
        m.Name = "Joint"; m.Icon = ICON_FA_LINK; m.Category = "Physics";
        m.Tooltip = "Constrains this body to another (or the world) while playing. Needs a Rigidbody\n"
                    "on this object and, unless connected to the world, on the target too.";
        m.GenericSerialize = false;
        m.Key = "joint"; // matches the hand-written SceneSerializer JSON key (#185 PR 11) — see ReflectComponent::Key
        m.Fields = {
            { "Type", T::Enum, TARTARUS_REFLECT_FIELD(JointComponent, Kind), 0.0f,
              "Fixed weld; Hinge/Slider use the Axis; Ball is a point pivot; Distance is a leash." },
            { "Anchor", T::Vec3, TARTARUS_REFLECT_FIELD(JointComponent, Anchor), 0.05f,
              "Joint point in this object's local space." },
            { "Break Force", T::Float, TARTARUS_REFLECT_FIELD(JointComponent, BreakForce), 1.0f,
              "Force (N) that snaps the joint. 0 = unbreakable.", 0.0f, 1.0e6f, false, false, "%.0f" },
            { "Break Torque", T::Float, TARTARUS_REFLECT_FIELD(JointComponent, BreakTorque), 1.0f,
              "Torque (N\xC2\xB7m) that snaps the joint. 0 = unbreakable.", 0.0f, 1.0e6f, false, false, "%.0f" },
            { "ConnectedOrder", T::Int, TARTARUS_REFLECT_FIELD(JointComponent, ConnectedOrder) },
            { "Axis", T::Vec3, TARTARUS_REFLECT_FIELD(JointComponent, Axis), 0.02f },
            { "UseLimit", T::Bool, TARTARUS_REFLECT_FIELD(JointComponent, UseLimit) },
            { "LimitLower", T::Float, TARTARUS_REFLECT_FIELD(JointComponent, LimitLower), 0.5f },
            { "LimitUpper", T::Float, TARTARUS_REFLECT_FIELD(JointComponent, LimitUpper), 0.5f },
        };
        auto F = [&](const char* name) -> ReflectField& {
            for (auto& f : m.Fields) if (std::strcmp(f.Name, name) == 0) return f;
            return m.Fields[0];
        };
        F("Type").EnumLabels = "Fixed\0Hinge\0Ball\0Slider\0Distance\0"; F("Type").EnumCount = 5;
        for (const char* h : { "ConnectedOrder", "Axis", "UseLimit", "LimitLower", "LimitUpper" })
            F(h).EditorHidden = true;
        Register<JointComponent>(std::move(m));
    }

    // #315 — Mesh Renderer. Registered so the component-registration guard rail counts it and
    // its Icon/Category live here, but BOTH generic paths are opted out: RenderableComponent
    // holds a shared_ptr<Model> (no reflectable fields), its scene form is the box "color/size"
    // or model "path" + "material" written by dedicated code in SceneSerializer, and its add
    // stashes/restores the mesh via DetachedMeshComponent using the editor-only AssetLibrary.
    // The Inspector section (mesh picker, primitive popup, drag-drop, ping, animation controls,
    // the level-geometry Color row) stays hand-coded in EditorLayer_Inspector.cpp, permanently —
    // settled #6 item 3, not a deferred follow-up: ModelRef is a live shared_ptr<Model>, not an
    // AssetRef-shaped path, so genericising it would mean rewriting RenderableComponent's storage,
    // not just its Inspector widget. (ComponentRegistry can't depend on src/Editor's AssetLibrary
    // either, so a new reflected field type for it would carry no drawable metadata anyway.)
    {
        ReflectComponent m;
        m.Name = "Mesh Renderer"; m.Icon = ICON_FA_DRAW_POLYGON; m.Category = "Rendering";
        m.Tooltip = "The mesh this object draws, and its material color/texture options.";
        m.GenericSerialize = false;
        m.GenericInspector = false;
        Register<RenderableComponent>(std::move(m));
    }

    // Lane P: scene-level visual effects and HUD settings

    Register<FxHudSettingsComponent>({
        "FX & HUD Settings", ICON_FA_SLIDERS,
        "Scene-level laser and HUD display parameters. Weapons own their muzzle effects;\n"
        "legacy muzzle values remain saved for older scenes.",
        "Gameplay",
        {
            { "Flash Time", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, FlashTime), 0.001f,
              "Seconds the muzzle flash light stays on.", 0.001f, 1.0f },
            { "Player Flash Scale", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, PlayerFlashScale), 0.01f,
              "Player's flash light scale relative to soldier's.", 0.01f, 2.0f },
            { "Flame Glow", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, FlameGlow), 5.0f,
              "Flame peak emission intensity (red channel).", 0.0f, 1000.0f },
            { "Muzzle Style", T::Int, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, MuzzleStyle), 1.0f,
              "0: the Tactical Shooter flame alone. 1: plus PRO Effects' flash (star / burst), side jets, core glow, gas puff and barrel smoke "
              "(needs the Knife effects data in assets/Effects/Knife).", 0.0f, 1.0f },
            { "Flame Scale", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, FlameScale), 0.05f,
              "Flame tongue length/width scale vs. tactical shooter pack.", 0.1f, 5.0f },
            { "Beam Range", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, BeamRange), 1.0f,
              "Laser beam metres drawn before fading in the haze.", 1.0f, 500.0f },
            { "Beam Half Width", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, BeamHalfWidth), 0.0001f,
              "Laser beam width in metres.", 0.0001f, 0.1f },
            { "Beam Falloff", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, BeamFalloff), 0.1f,
              "Laser beam glow falloff distance in metres near the emitter.", 0.1f, 50.0f },
            { "Beam Bend", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, BeamBend), 0.1f,
              "Laser beam easing distance over which view-model emitter eases onto the true path (metres).", 0.1f, 20.0f },
        },
    });
    for(size_t i=0;i<5;++i)Storage().back().Meta.Fields[i].EditorHidden=true;

    Register<BloodSettingsComponent>({
        "Blood Settings", ICON_FA_DROPLET,
        "The volumetric blood (the first Blood Settings in the scene counts): how big the sprays are,\n"
        "how many stains stay and for how long, and what blood lands on.",
        "Gameplay",
        {
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, Enabled), 0.0f,
              "Blood at all." },
            { "Size", T::Float, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, Size), 0.05f,
              "x every spray's size; its fall time follows (by the square root), so gravity stays right.", 0.2f, 3.0f },
            { "Max Sprays", T::Int, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, MaxSprays), 1.0f,
              "Airborne sprays at once; past this the oldest is dropped.", 1.0f, 128.0f },
            { "Max Stains", T::Int, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, MaxStains), 1.0f,
              "Stains on the world at once; past this the farthest one out of view goes.", 8.0f, 2048.0f },
            { "Stain Lifetime", T::Float, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, StainLifetime), 1.0f,
              "Seconds a stain stays; it only ever leaves out of view (behind you or far off). 0 = for good.", 0.0f, 3600.0f },
            { "Dry Seconds", T::Float, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, DrySeconds), 1.0f,
              "Seconds for fresh, wet blood to dry dark and matte, counted only while it's out of view (pools take 2.5x, cloth 1.5x).", 5.0f, 7200.0f },
            { "Speed", T::Float, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, Speed), 0.05f,
              "x how fast the sprays play and the blood lands (1 = the packs' cinematic timing; 2 = the default).", 0.5f, 3.0f },
            { "Pools", T::Bool, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, Pools), 0.0f,
              "A pool spreads under each corpse." },
            { "Body Splats", T::Bool, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, BodySplats), 0.0f,
              "Blood on bodies, ragdolls and loose props." },
            { "Gear Spatter", T::Bool, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, GearSpatter), 0.0f,
              "Point-blank blood on the player's gun and hands, and their own wounds." },
            { "Energy Scale", T::Float, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, EnergyScale), 0.05f,
              "x every hit's energy: how far and fast the spray is thrown, how much mist, whether rounds come out the far side.", 0.2f, 3.0f },
            { "Impact Puffs", T::Bool, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, ImpactPuffs), 0.0f,
              "The burst, mist and droplets on the frame a round goes in (needs the Knife effects data in assets/Effects/Knife)." },
            { "Gore", T::Int, TARTARUS_REFLECT_FIELD(BloodSettingsComponent, Gore), 1.0f,
              "0 off (no sprays or stains), 1 on.", 0.0f, 1.0f },
        },
    });
}

} // namespace ComponentRegistry
