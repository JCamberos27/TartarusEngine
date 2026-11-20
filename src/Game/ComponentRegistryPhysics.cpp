#include "ComponentRegistryInternal.h"

#include "Components.h"
#include "ReflectAttributes.h" // vInspector attributes (Attr)

#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <random>

namespace ComponentRegistry {

// Ragdoll settings.
void RegisterPhysicsComponents() {
    using T = ReflectFieldType;

    {
        ReflectComponent m;
        m.Name = "Ragdoll Settings"; m.Icon = ICON_FA_PERSON_FALLING; m.Category = "AI";
        m.Tooltip = "How dead soldiers' ragdolls are built and behave (the first Ragdoll Settings in the scene counts).\nJoint limits are degrees from the neutral pose.";
        m.Fields = {
            { "Anatomical Limits", T::Bool, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, AnatomicalLimits), 0.0f,
              "Joints use human ranges of motion (knees and elbows are one-way hinges). Off: the old generous symmetric cones, which let knees and elbows bend backwards." },
            { "Drive Stiffness", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, DriveStiffness), 5.0f,
              "Strength of the joint drives that hold the death pose (acceleration units, so independent of mass). 0 = limp at once.", 0.0f, 5000.0f },
            { "Drive Damping", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, DriveDamping), 1.0f,
              "Damping of those drives.", 0.0f, 500.0f },
            { "Drive Fade", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, DriveFade), 0.01f,
              "Seconds from death until the drives are off and the body is limp (squared fade).", 0.01f, 5.0f },
            { "Linear Damping", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, LinearDamping), 0.005f,
              "Air drag on every part.", 0.0f, 5.0f },
            { "Angular Damping", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, AngularDamping), 0.005f,
              "Rotational drag on every part.", 0.0f, 10.0f },
            { "Solver Position Iterations", T::Int, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SolverPosIters), 1.0f,
              "Joint accuracy: more is stiffer and costs more per corpse.", 1.0f, 64.0f },
            { "Solver Velocity Iterations", T::Int, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SolverVelIters), 1.0f,
              "Velocity-solver iterations per part.", 1.0f, 64.0f },
            { "Depenetration Speed", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, Depenetration), 0.1f,
              "Max speed (m/s) at which overlapping parts are pushed apart.", 0.0f, 50.0f },
            { "Sleep Threshold", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SleepThreshold), 0.005f,
              "Kinetic energy per mass below which a part sleeps; a body is at rest when all parts sleep.", 0.0f, 2.0f },
            { "Static Friction", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, StaticFriction), 0.01f,
              "Part against world.", 0.0f, 4.0f },
            { "Dynamic Friction", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, DynamicFriction), 0.01f,
              "Part against world, sliding.", 0.0f, 4.0f },
            { "Restitution", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, Restitution), 0.01f,
              "Bounciness of a part.", 0.0f, 1.0f },
            { "Part Impulse Speed", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, PartImpulseSpeed), 0.1f,
              "A shot's shove on the struck part is capped at mass x this (m/s), so a light forearm is not torn off.", 0.1f, 50.0f },
            { "Chest Impulse Speed", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ChestImpulseSpeed), 0.1f,
              "The remainder of the shove goes into the chest, capped at chest mass x this (m/s).", 0.0f, 50.0f },
            { "Corpse Shot Base", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CorpseShotBase), 0.05f,
              "A shot into a corpse shoves the part with mass x (this + Per Damage x damage) N s.", 0.0f, 20.0f },
            { "Corpse Shot Per Damage", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CorpseShotPerDamage), 0.005f,
              "See Corpse Shot Base.", 0.0f, 1.0f },
            { "Pelvis Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, PelvisMass), 0.1f,
              "kg. The whole body is about 75 kg.", 0.1f, 200.0f },
            { "Spine Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Spine Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Spine Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Spine Lateral In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineLatIn), 0.5f,
              "Degrees toward the body's midline (adduction); a spine or head leans this far either way.", 0.0f, 175.0f },
            { "Spine Lateral Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineLatOut), 0.5f,
              "Degrees away from the midline (abduction).", 0.0f, 175.0f },
            { "Spine Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Spine Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Head Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Head Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Head Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Head Lateral In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadLatIn), 0.5f,
              "Degrees toward the body's midline (adduction); a spine or head leans this far either way.", 0.0f, 175.0f },
            { "Head Lateral Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadLatOut), 0.5f,
              "Degrees away from the midline (abduction).", 0.0f, 175.0f },
            { "Head Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Head Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Upper Arm Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Upper Arm Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Upper Arm Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Upper Arm Lateral In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmLatIn), 0.5f,
              "Degrees toward the body's midline (adduction); a spine or head leans this far either way.", 0.0f, 175.0f },
            { "Upper Arm Lateral Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmLatOut), 0.5f,
              "Degrees away from the midline (abduction).", 0.0f, 175.0f },
            { "Upper Arm Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Upper Arm Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Forearm Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Forearm Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Forearm Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Forearm Sideways In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmLatIn), 0.5f,
              "Degrees of sideways wobble the hinge allows (slack so the joint doesn't bind).", 0.0f, 175.0f },
            { "Forearm Sideways Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmLatOut), 0.5f,
              "Degrees of sideways wobble the hinge allows (slack so the joint doesn't bind).", 0.0f, 175.0f },
            { "Forearm Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Forearm Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Thigh Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Thigh Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Thigh Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Thigh Lateral In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighLatIn), 0.5f,
              "Degrees toward the body's midline (adduction); a spine or head leans this far either way.", 0.0f, 175.0f },
            { "Thigh Lateral Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighLatOut), 0.5f,
              "Degrees away from the midline (abduction).", 0.0f, 175.0f },
            { "Thigh Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Thigh Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Calf Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Calf Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Calf Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Calf Sideways In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfLatIn), 0.5f,
              "Degrees of sideways wobble the hinge allows (slack so the joint doesn't bind).", 0.0f, 175.0f },
            { "Calf Sideways Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfLatOut), 0.5f,
              "Degrees of sideways wobble the hinge allows (slack so the joint doesn't bind).", 0.0f, 175.0f },
            { "Calf Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Calf Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Limb Velocity Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, LimbVelocityScale), 0.05f,
              "How much of each bone's own motion (from the last two animated poses) the parts keep at death: a soldier shot mid-stride keeps his swinging limbs. 0 = only the body's velocity.", 0.0f, 2.0f },
            { "Max Limb Speed", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, MaxLimbSpeed), 0.1f,
              "Cap on a part's speed relative to the body from that (m/s); stops a teleport or a bad first frame throwing a limb.", 0.0f, 30.0f },
            { "Max Limb Spin", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, MaxLimbSpin), 0.5f,
              "Cap on a part's spin from that (rad/s).", 0.0f, 100.0f },
            { "Shaped Torso Inertia", T::Bool, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ShapedTorsoInertia), 0.0f,
              "Pelvis and chest turn like a box wider than deep (a human trunk) instead of a round capsule. Off: the capsule's own inertia." },
            { "Torso Half Width", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, TorsoHalfWidth), 0.005f,
              "Half the trunk's width (shoulder to shoulder), for its inertia only; hitboxes are unchanged.", 0.05f, 0.4f },
            { "Torso Half Depth", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, TorsoHalfDepth), 0.005f,
              "Half the trunk's depth (chest to back), for its inertia only.", 0.05f, 0.3f },
            { "Inertia Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, InertiaScale), 0.05f,
              "Multiplies every part's rotational inertia. Above 1 the parts turn more slowly (stabler), below 1 they whip about.", 0.2f, 5.0f },
            { "Pelvis Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, PelvisFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Spine Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Head Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Upper Arm Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Forearm Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Thigh Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Calf Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
        };
        m.Fields[0].Group = "Joints";
        m.Fields[1].Group = "Death Drive";
        m.Fields[2].Group = "Death Drive";
        m.Fields[3].Group = "Death Drive";
        m.Fields[4].Group = "Body Physics";
        m.Fields[5].Group = "Body Physics";
        m.Fields[6].Group = "Body Physics";
        m.Fields[7].Group = "Body Physics";
        m.Fields[8].Group = "Body Physics";
        m.Fields[9].Group = "Body Physics";
        m.Fields[10].Group = "Contact";
        m.Fields[11].Group = "Contact";
        m.Fields[12].Group = "Contact";
        m.Fields[13].Group = "Impulse Caps";
        m.Fields[14].Group = "Impulse Caps";
        m.Fields[15].Group = "Impulse Caps";
        m.Fields[16].Group = "Impulse Caps";
        m.Fields[17].Group = "Pelvis";
        m.Fields[18].Group = "Spine";
        m.Fields[19].Group = "Spine";
        m.Fields[20].Group = "Spine";
        m.Fields[21].Group = "Spine";
        m.Fields[22].Group = "Spine";
        m.Fields[23].Group = "Spine";
        m.Fields[24].Group = "Spine";
        m.Fields[25].Group = "Head";
        m.Fields[26].Group = "Head";
        m.Fields[27].Group = "Head";
        m.Fields[28].Group = "Head";
        m.Fields[29].Group = "Head";
        m.Fields[30].Group = "Head";
        m.Fields[31].Group = "Head";
        m.Fields[32].Group = "Upper Arm";
        m.Fields[33].Group = "Upper Arm";
        m.Fields[34].Group = "Upper Arm";
        m.Fields[35].Group = "Upper Arm";
        m.Fields[36].Group = "Upper Arm";
        m.Fields[37].Group = "Upper Arm";
        m.Fields[38].Group = "Upper Arm";
        m.Fields[39].Group = "Forearm";
        m.Fields[40].Group = "Forearm";
        m.Fields[41].Group = "Forearm";
        m.Fields[42].Group = "Forearm";
        m.Fields[43].Group = "Forearm";
        m.Fields[44].Group = "Forearm";
        m.Fields[45].Group = "Forearm";
        m.Fields[46].Group = "Thigh";
        m.Fields[47].Group = "Thigh";
        m.Fields[48].Group = "Thigh";
        m.Fields[49].Group = "Thigh";
        m.Fields[50].Group = "Thigh";
        m.Fields[51].Group = "Thigh";
        m.Fields[52].Group = "Thigh";
        m.Fields[53].Group = "Calf";
        m.Fields[54].Group = "Calf";
        m.Fields[55].Group = "Calf";
        m.Fields[56].Group = "Calf";
        m.Fields[57].Group = "Calf";
        m.Fields[58].Group = "Calf";
        m.Fields[59].Group = "Calf";
        m.Fields[60].Group = "Death Momentum";
        m.Fields[61].Group = "Death Momentum";
        m.Fields[62].Group = "Death Momentum";
        m.Fields[63].Group = "Inertia";
        m.Fields[64].Group = "Inertia";
        m.Fields[65].Group = "Inertia";
        m.Fields[66].Group = "Inertia";
        m.Fields[67].Group = "Death Drive";
        m.Fields[68].Group = "Death Drive";
        m.Fields[69].Group = "Death Drive";
        m.Fields[70].Group = "Death Drive";
        m.Fields[71].Group = "Death Drive";
        m.Fields[72].Group = "Death Drive";
        m.Fields[73].Group = "Death Drive";
        {
            // The neck, hands and feet: ragdoll-only parts (the hitboxes are unchanged).
            struct R { const char* Label; void* (*Ptr)(void*); float Step; const char* Tip; float Lo, Hi; const char* Group; };
            const R rows[] = {
                { "Neck Mass", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, NeckMass), 0.1f, "kg. A ragdoll-only part between the chest and the head.", 0.1f, 200.0f, "Neck" },
                { "Neck Flexion Max", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, NeckFlexMax), 0.5f, "Degrees the part can swing in its flexion direction (forward), from its neutral pose.", 0.0f, 175.0f, "Neck" },
                { "Neck Extension Max", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, NeckExtMax), 0.5f, "Degrees it can swing the other way (back).", 0.0f, 175.0f, "Neck" },
                { "Neck Lateral In", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, NeckLatIn), 0.5f, "Degrees toward the body's midline (adduction). A neck leans this far either way.", 0.0f, 175.0f, "Neck" },
                { "Neck Lateral Out", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, NeckLatOut), 0.5f, "Degrees away from the midline (abduction).", 0.0f, 175.0f, "Neck" },
                { "Neck Twist In", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, NeckTwistIn), 0.5f, "Degrees of inward rotation along the bone.", 0.0f, 175.0f, "Neck" },
                { "Neck Twist Out", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, NeckTwistOut), 0.5f, "Degrees of outward rotation along the bone.", 0.0f, 175.0f, "Neck" },
                { "Hand Mass", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HandMass), 0.1f, "kg, each side. A ragdoll-only part (the forearm's hitbox still covers the hand).", 0.1f, 200.0f, "Hand" },
                { "Hand Flexion Max", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HandFlexMax), 0.5f, "Degrees the part can swing in its flexion direction (the palm side (wrist flexion)), from its neutral pose.", 0.0f, 175.0f, "Hand" },
                { "Hand Extension Max", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HandExtMax), 0.5f, "Degrees it can swing the other way (the back of the hand (wrist extension)).", 0.0f, 175.0f, "Hand" },
                { "Hand Lateral In", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HandLatIn), 0.5f, "Degrees toward the body's midline (adduction).", 0.0f, 175.0f, "Hand" },
                { "Hand Lateral Out", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HandLatOut), 0.5f, "Degrees away from the midline (abduction).", 0.0f, 175.0f, "Hand" },
                { "Hand Twist In", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HandTwistIn), 0.5f, "Degrees of inward rotation along the bone.", 0.0f, 175.0f, "Hand" },
                { "Hand Twist Out", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HandTwistOut), 0.5f, "Degrees of outward rotation along the bone.", 0.0f, 175.0f, "Hand" },
                { "Foot Mass", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FootMass), 0.1f, "kg, each side. A ragdoll-only part (the calf's hitbox still covers the foot).", 0.1f, 200.0f, "Foot" },
                { "Foot Flexion Max", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FootFlexMax), 0.5f, "Degrees the part can swing in its flexion direction (toes up (dorsiflexion)), from its neutral pose.", 0.0f, 175.0f, "Foot" },
                { "Foot Extension Max", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FootExtMax), 0.5f, "Degrees it can swing the other way (toes down (plantarflexion)).", 0.0f, 175.0f, "Foot" },
                { "Foot Lateral In", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FootLatIn), 0.5f, "Degrees toward the body's midline (adduction).", 0.0f, 175.0f, "Foot" },
                { "Foot Lateral Out", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FootLatOut), 0.5f, "Degrees away from the midline (abduction).", 0.0f, 175.0f, "Foot" },
                { "Foot Twist In", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FootTwistIn), 0.5f, "Degrees of inward rotation along the bone (foot: inversion).", 0.0f, 175.0f, "Foot" },
                { "Foot Twist Out", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FootTwistOut), 0.5f, "Degrees of outward rotation along the bone (foot: eversion).", 0.0f, 175.0f, "Foot" },
                { "Neck Fade Scale", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, NeckFadeScale), 0.05f, "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f, "Death Drive" },
                { "Hand Fade Scale", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HandFadeScale), 0.05f, "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f, "Death Drive" },
                { "Foot Fade Scale", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FootFadeScale), 0.05f, "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f, "Death Drive" },
            };
            for (const R& r : rows) {
                m.Fields.push_back({ r.Label, T::Float, r.Ptr, r.Step, r.Tip, r.Lo, r.Hi });
                m.Fields.back().Group = r.Group;
            }
        }
        {
            // Hit flinch (see NpcFlinch).
            m.Fields.push_back({ "Hit Flinch", T::Bool, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HitFlinch), 0.0f,
              "A round that doesn't kill kicks the struck region (a damped spring on top of the hit animation) and it settles back. Visual only: aim and hitboxes don't move." });
            m.Fields.push_back({ "Flinch Angle", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FlinchAngle), 0.25f,
              "Degrees the struck bone kicks at the reference damage (each region scales it: a head or an arm kicks further than the chest).", 0.0f, 60.0f });
            m.Fields.push_back({ "Flinch Duration", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FlinchDuration), 0.01f,
              "Seconds until the kick has settled back to the animation.", 0.05f, 2.0f });
            m.Fields.push_back({ "Flinch Damage Reference", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FlinchDamageRef), 1.0f,
              "The damage that gives the full Flinch Angle; lighter rounds kick proportionally less (down to 30%), heavier up to twice as much.", 1.0f, 500.0f });
            m.Fields.push_back({ "Flinch Max Angle", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FlinchMaxAngle), 0.5f,
              "Cap on a bone's total kick (degrees) when rounds land in quick succession.", 1.0f, 90.0f });
            for (size_t k = m.Fields.size() - 5; k < m.Fields.size(); ++k) m.Fields[k].Group = "Hit Flinch";
            m.Fields.push_back({ "Distal Inertia Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, DistalInertiaScale), 0.1f,
              "Multiplies the hands' and feet's rotational inertia (on top of Inertia Scale). Light end links whip the forearm or calf through its joint limit; 1 = off.", 0.2f, 40.0f });
            m.Fields.push_back({ "Distal Joint Damping", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, DistalJointDamping), 1.0f,
              "Viscous damping of the wrist and ankle joints (acceleration units), kept on after the death drives fade. Keeps the hands and feet from whipping the limb above; 0 = off.", 0.0f, 500.0f });
            m.Fields[m.Fields.size() - 2].Group = "Body Physics";
            m.Fields[m.Fields.size() - 1].Group = "Body Physics";
            m.Fields.push_back({ "Gravity Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, GravityScale), 0.01f,
              "Gravity on a dead body as a multiple of the scene's. At 1 a body falls true to scale but reads light on screen; a little more gives it weight.", 0.5f, 3.0f });
            m.Fields.push_back({ "Skin Offset", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SkinOffset), 0.001f,
              "Metres of flesh over the collision capsules: contacts rest this far out, so the skinned body lies on the floor instead of sinking into it.", 0.0f, 0.06f });
            m.Fields.push_back({ "Self Collision", T::Bool, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SelfCollision), 0.0f,
              "The body's parts collide with each other (never a part with the one it hangs from): an arm can't pass through the chest, a leg through the other. A pair already overlapping when the body goes down is let through until it has come apart." });
            m.Fields.push_back({ "Twist Armature", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, TwistArmature), 0.01f,
              "Inertia added to each joint's twist, as a fraction of what the joint turns. A limb has almost none about its own length, so friction against the floor or a crate spins it past its twist limit; 0 = off.", 0.0f, 3.0f });
            m.Fields.push_back({ "Swing Armature", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SwingArmature), 0.01f,
              "Inertia added to each joint's bend (the swing axes, and the elbows' and knees' hinge), as a fraction of what the joint turns; 0 = off.", 0.0f, 3.0f });
            m.Fields.push_back({ "Hand Friction", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HandFriction), 0.01f,
              "The hands' friction as a fraction of the body's: a hand slides off a stair, ledge or crate edge instead of hooking on it and being bent past its range by the body hanging from it. 1 = as the body.", 0.0f, 1.0f });
            m.Fields.push_back({ "Finger Relax Time", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FingerRelaxTime), 0.01f,
              "Seconds for a dead hand's fingers to let go of the grip they died in (a weapon grip would otherwise stay clawed round a gun that has fallen). 0 = at once.", 0.0f, 3.0f });
            m.Fields.push_back({ "Finger Curl", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, FingerCurl), 0.01f,
              "How much of the grip a dead hand's fingers keep: 0 = straight (the rig's bind pose), 1 = the grip as it was.", 0.0f, 1.0f });
            for (size_t k = m.Fields.size() - 8; k < m.Fields.size(); ++k) m.Fields[k].Group = "Body Physics";
        }
        {
            // The powered ragdoll (NpcRagdollMotor).
            m.Fields.push_back({ "Powered Ragdoll", T::Bool, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, PoweredRagdoll), 0.0f,
              "Dead soldiers keep muscle tone: every joint holds a spring toward the pose it died in, blending into a collapse, with the strength decaying per region (legs first, then spine, neck and arms) to a small residual. The round carries the body, and a settled body comes to rest and sleeps. Off: the old behaviour (Death Drive fade, then a rag)." });
            m.Fields.back().Group = "Muscle Tone";
            m.Fields.push_back({ "Grip Floor", T::Bool, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, GripFloor), 0.0f,
              "The body's friction wins against a slicker floor material and it never bounces (max-combined friction, multiplied restitution). Only with Powered Ragdoll." });
            m.Fields.back().Group = "Settle";
            struct R2 { const char* Group; const char* Label; void* (*Ptr)(void*); float Step; const char* Tip; float Lo, Hi; };
            const R2 rows2[] = {
                { "Muscle Tone", "Tone Stiffness", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ToneStiffness), 5.0f, "Spring strength of every joint at full muscle strength (acceleration units: mass independent). The pose is held this hard until the region's tone decays.", 0.0f, 5000.0f },
                { "Muscle Tone", "Tone Damping", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ToneDamping), 1.0f, "Damper of that spring at full strength.", 0.0f, 500.0f },
                { "Muscle Tone", "Tone Residual", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ToneResidual), 0.005f, "Fraction of the strength a dead body keeps for good (squared into stiffness), so a limp body is never a free noodle. 0 = fully limp.", 0.0f, 0.5f },
                { "Muscle Tone", "Joint Friction", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, JointFriction), 0.5f, "Damper floor on every joint (acceleration units), always on: dead bodies have joint friction. 0 = frictionless joints.", 0.0f, 200.0f },
                { "Muscle Tone", "Legs Tone Time", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, LegsToneTime), 0.05f, "Seconds for the legs' muscle strength to decay to the residual (after the Stagger Time).", 0.05f, 6.0f },
                { "Muscle Tone", "Spine Tone Time", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineToneTime), 0.05f, "Seconds for the spine's strength to decay to the residual.", 0.05f, 6.0f },
                { "Muscle Tone", "Neck Tone Time", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, NeckToneTime), 0.05f, "Seconds for the neck's and head's strength to decay to the residual.", 0.05f, 6.0f },
                { "Muscle Tone", "Arms Tone Time", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ArmsToneTime), 0.05f, "Seconds for the arms' and hands' strength to decay to the residual.", 0.05f, 6.0f },
                { "Muscle Tone", "Collapse Blend Time", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CollapseBlendTime), 0.05f, "Seconds the joints' target takes to go from the pose the soldier died in to the collapse pose.", 0.05f, 4.0f },
                { "Muscle Tone", "Collapse Amount", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CollapseAmount), 0.05f, "0 = the joints keep aiming for the death pose, 1 = the full collapse pose below (knees and hips fold, spine curls, arms drop or brace).", 0.0f, 1.5f },
                { "Muscle Tone", "Hip Flexion", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HipFlexCollapse), 0.5f, "Degrees the hips fold in the collapse pose.", 0.0f, 120.0f },
                { "Muscle Tone", "Knee Flexion", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, KneeFlexCollapse), 0.5f, "Degrees the knees fold in the collapse pose.", 0.0f, 140.0f },
                { "Muscle Tone", "Spine Curl", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineCurlCollapse), 0.5f, "Degrees the spine curls (forward when the body falls forward, a little back when it falls back).", 0.0f, 45.0f },
                { "Muscle Tone", "Neck Tuck", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, NeckCollapse), 0.5f, "Degrees the neck tucks (chin in when falling back, head back when falling forward).", 0.0f, 50.0f },
                { "Muscle Tone", "Shoulder Brace", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ShoulderCollapse), 0.5f, "Degrees the arms come forward to brace a fall forward (a fall back throws them back, a sideways one swings them out: see Reactions, where Brace Weight scales all of it).", 0.0f, 120.0f },
                { "Muscle Tone", "Elbow Flexion", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ElbowCollapse), 0.5f, "Degrees the elbows fold in the collapse pose.", 0.0f, 140.0f },
                { "Stagger", "Stagger Leg Strength", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, StaggerLegStrength), 0.02f, "The legs start at this fraction of the muscle strength: they give out instead of holding the pose, so the body topples and doesn't stand rigid.", 0.0f, 1.0f },
                { "Stagger", "Stagger Time", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, StaggerTime), 0.01f, "Seconds the legs hold that strength before their tone starts to decay: the body stays on its feet long enough for the round to carry it.", 0.0f, 2.0f },
                { "Stagger", "Hit Weakness", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HitWeakness), 0.01f, "Strength fraction of the joint the round struck (a struck chest or head gives way; a struck leg buckles). 1 = no different from the rest.", 0.0f, 1.0f },
                { "Stagger", "Hit Impulse Scale", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HitImpulseScale), 0.05f, "Multiplies the round's shove on the body: the body falls along the shot. 1 = the plain impulse.", 0.0f, 10.0f },
                { "Stagger", "Hit Body Share", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HitBodyShare), 0.02f, "Share of the shove that pushes the whole body (every part by mass: it is carried along the shot and topples on its feet) rather than only the struck part (which folds around the round). 0 = all on the struck part.", 0.0f, 1.0f },
                { "Settle", "Impact Settle Delay", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ImpactSettleDelay), 0.02f, "Seconds after the pelvis or chest first meets the ground before the body may settle (damping and friction ramp up): the limbs slump and roll on with their own weight first instead of freezing mid-fall.", 0.0f, 3.0f },
                { "Settle", "Settle Speed", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SettleSpeed), 0.02f, "Fastest part speed (m/s) under which the body starts to settle (damping and friction ramp up).", 0.01f, 5.0f },
                { "Settle", "Settle Delay", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SettleDelay), 0.05f, "Seconds after death before the body may start to settle.", 0.0f, 5.0f },
                { "Settle", "Settle Ramp", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SettleRamp), 0.05f, "Seconds the damping, friction and joint friction take to reach their settled values.", 0.05f, 5.0f },
                { "Settle", "Settle Linear Damping", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SettleLinearDamping), 0.05f, "Linear damping of every part once settled.", 0.0f, 20.0f },
                { "Settle", "Settle Angular Damping", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SettleAngularDamping), 0.05f, "Angular damping of every part once settled.", 0.0f, 40.0f },
                { "Settle", "Settle Friction", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SettleFriction), 0.05f, "Static and dynamic friction against the world once settled (no sliding on the floor).", 0.0f, 6.0f },
                { "Settle", "Settle Joint Friction", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SettleJointFriction), 0.5f, "The joint damper floor once settled (acceleration units): no limb jitter at rest.", 0.0f, 200.0f },
                { "Settle", "Rest Speed", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RestSpeed), 0.001f, "Every part slower than this (m/s; rad/s at ten times) for Rest Time and the body is put to sleep, and stays so until hit.", 0.0f, 1.0f },
                { "Settle", "Rest Time", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RestTime), 0.05f, "Seconds of stillness before the body is put to sleep.", 0.05f, 5.0f },
                { "Settle", "Creep Speed", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CreepSpeed), 0.005f, "m/s (and rad/s x 20): a settled body moving no faster than this for Rest Timeout is put to sleep anyway.", 0.0f, 1.0f },
                { "Settle", "Rest Timeout", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RestTimeout), 0.05f, "Seconds a settled body may go on creeping (a gun or another body lying on it, a hand caught at two joint limits) before it is put to sleep anyway. 0 = off.", 0.0f, 10.0f },
                { "Settle", "Stabilization Threshold", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, StabilizationThreshold), 0.005f, "PhysX stabilization: a part with less kinetic energy per mass than this is held still against contact jitter.", 0.0f, 2.0f },
                { "Lying", "Relax Delay", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RelaxDelay), 0.02f, "Seconds after death before the joints' target starts going from the collapse pose to the lying pose.", 0.0f, 4.0f },
                { "Lying", "Relax Loose", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RelaxLoose), 0.02f, "The most the settle (joint friction, floor friction and damping ramping up) may reach until the lying pose has formed, so the limbs can still move on the floor. 1 = no limit.", 0.0f, 1.0f },
                { "Lying", "Relax Time", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RelaxTime), 0.05f, "Seconds the joints' target takes to go from the collapse pose to a relaxed lying pose (legs extend, hips and arms relax), (after the Relax Delay). A body no longer ends frozen kneeling or curled up.", 0.05f, 5.0f },
                { "Lying", "Relax Tone", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RelaxTone), 0.02f, "Strength fraction that holds the lying pose while it forms. 0 = no lying phase: the target stays at the collapse pose and the body is as limp as the Tone Residual lets it be.", 0.0f, 1.0f },
                { "Lying", "Relax Hold", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RelaxHold), 0.05f, "Seconds the lying tone holds after the target has arrived, then fades out (the body may not sleep before that).", 0.0f, 5.0f },
                { "Lying", "Rest Pelvis Max", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RestPelvisMax), 0.01f, "A body about to rest whose pelvis is higher than this (m) is kneeling or propped up, not lying: it is not put to sleep yet and the settle lets go of it while its legs push toward the lying pose (see Rest Fix Time).", 0.05f, 1.0f },
                { "Lying", "Rest Chest Max", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RestChestMax), 0.01f, "A body about to rest whose chest is higher than this (m) is propped up on its arms or legs, not lying: same as Rest Pelvis Max.", 0.1f, 1.0f },
                { "Lying", "Rest Knee Max", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RestKneeMax), 1.0f, "A body about to rest with a knee bent more than this (degrees) is curled up: same as Rest Pelvis Max.", 10.0f, 140.0f },
                { "Lying", "Rest Hip Max", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RestHipMax), 1.0f, "A body about to rest with a hip folded more than this (degrees) is curled up: same as Rest Pelvis Max.", 10.0f, 150.0f },
                { "Lying", "Rest Fix Time", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RestFixTime), 0.05f, "The longest (seconds after the lying pose has formed) the legs push a kneeling or curled-up body toward the lying pose before it is left as it lies (propped against something, it stays so). 0 = never push.", 0.0f, 5.0f },
                { "Lying", "Rest Fix Tone", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, RestFixTone), 0.02f, "The legs' strength fraction while they push (and the settle is held to Relax Loose).", 0.0f, 1.0f },
                { "Lying", "Lying Hip Flexion", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, LyingHip), 0.5f, "Degrees the hips are flexed in the lying pose.", 0.0f, 90.0f },
                { "Lying", "Lying Knee Flexion", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, LyingKnee), 0.5f, "Degrees the knees are bent in the lying pose.", 0.0f, 90.0f },
                { "Lying", "Lying Elbow Flexion", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, LyingElbow), 0.5f, "Degrees the elbows are bent in the lying pose.", 0.0f, 120.0f },
                { "Lying", "Lying Arms", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, LyingArms), 0.02f, "How far the arms go back to hanging in the lying pose (0 = they keep the pose they died in, 1 = straight down).", 0.0f, 1.0f },
                { "Lying", "Lying Spine", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, LyingSpine), 0.02f, "How far the spine, neck and head go back to upright in the lying pose.", 0.0f, 1.0f },
                { "Directional Fall", "Buckle Asymmetry", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, BuckleAsymmetry), 0.02f, "The leg on the side the body falls toward (the far side from the shot) folds its knee further and the other less. 0 = both knees fold alike.", 0.0f, 1.0f },
                { "Directional Fall", "Buckle Lead", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, BuckleLead), 0.02f, "That lead leg gives out earlier: its Stagger Time hold is shortened by this fraction (the other leg's lengthened).", 0.0f, 0.9f },
                { "Directional Fall", "Back Fall Knee Scale", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, BackKneeScale), 0.02f, "Knee fold of a body falling straight back, as a fraction of Knee Flexion (a back fall sits down more than it kneels).", 0.0f, 1.5f },
                { "Directional Fall", "Back Fall Hip Scale", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, BackHipScale), 0.02f, "Hip fold of a body falling straight back, as a multiple of Hip Flexion (the legs go forward as it sits).", 0.5f, 3.0f },
                { "Reactions", "Brace Weight", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, BraceWeight), 0.02f, "The arms reach toward the fall (forward, back, or out to the side), scaled by this. 0 = they keep the pose they died in. A head kill never braces.", 0.0f, 1.0f },
                { "Reactions", "Brace Sideways", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, BraceLateral), 0.5f, "Degrees the arms swing toward a sideways fall (before Brace Weight).", 0.0f, 90.0f },
                { "Reactions", "Head Tuck Weight", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadTuckWeight), 0.02f, "The head tucks forward as the body falls back (Neck Tuck degrees, scaled by this). A head kill does not tuck.", 0.0f, 1.0f },
                { "Reactions", "Head Kill Limp", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadKillLimp), 0.02f, "A head kill: the arms' and neck's muscle times (Tone Time) are scaled by this, so the body goes limp at once. 1 = no different from a body kill.", 0.05f, 1.0f },
                { "Corpse Hits", "Corpse Wake Time", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CorpseWakeTime), 0.02f, "Seconds a body at rest stays loose (no settling, joint friction low) after a round hits it, so the limb the round struck visibly reacts before the body settles and sleeps again.", 0.0f, 3.0f },
                { "Corpse Hits", "Corpse Shot Max Speed", TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CorpseShotMaxSpeed), 0.1f, "The most speed (m/s) a round adds to the part it struck on a body at rest, however heavy the round.", 0.5f, 20.0f },
            };
            for (const R2& r : rows2) {
                m.Fields.push_back({ r.Label, T::Float, r.Ptr, r.Step, r.Tip, r.Lo, r.Hi });
                m.Fields.back().Group = r.Group;
            }
        }
        Register<RagdollSettingsComponent>(std::move(m));
    }
}

} // namespace ComponentRegistry
