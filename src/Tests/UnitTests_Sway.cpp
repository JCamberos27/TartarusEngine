#include "UnitTestSupport.h"
#include "../Game/SwayModifier.h"
#include "../Game/SwayModifierSerialization.h"
#include "../Game/FirstPersonProcedural.h"
#include "../Game/FirstPersonAnimation.h"
#include "../Game/IK.h"
#include <cmath>
#include <glm/gtx/euler_angles.hpp>

namespace {
glm::quat Euler(glm::vec3 v) {
    return glm::quat_cast(glm::yawPitchRoll(glm::radians(v.y),glm::radians(v.x),glm::radians(v.z)));
}
bool Same(glm::quat a,glm::quat b) { return 1-std::abs(glm::dot(a,b))<1e-5f; }
void Settings() {
    SwayModifierSettings s, back;
    CHECK(s.AimingSway.Position.Speed==glm::vec3(7));
    CHECK(s.AimingSway.Rotation.Speed==glm::vec3(15,20,15));
    CHECK(s.AimingSway.Rotation.Scale==glm::vec3(-2,2,-2));
    CHECK(s.MovementSway.Position.Clamp==glm::vec3(0));
    CHECK(s.MovementSway.Rotation.Clamp==glm::vec3(0));
    CHECK(s.AimingSway.AdsScale==0 && s.MovementSway.AdsScale==0);
    CHECK(s.AdsCurveScale==1 && s.AdsCurveSmoothing==10);
    s.MovementSway.Space=SwaySpace::WorldSpace;
    s.SpaceOffset=Euler({10,20,30});
    s.AimingSway.Position.Clamp={0.01f,0.02f,0.03f};
    std::string error;
    CHECK(SwayModifierFromJson(SwayModifierToJson(s),back,&error));
    CHECK(back.MovementSway.Space==s.MovementSway.Space && Same(back.SpaceOffset,s.SpaceOffset));
    CHECK(back.AimingSway.Position.Clamp==s.AimingSway.Position.Clamp);
    auto bad=SwayModifierToJson(s); bad["aimingSway"]["rotation"]["stiffness"][1]=-1;
    CHECK(!SwayModifierFromJson(bad,back,&error));
    CHECK(back.AimingSway.Position.Clamp==s.AimingSway.Position.Clamp);
    bad=SwayModifierToJson(s); bad["spaceOffset"]={0,0,0,0};
    CHECK(!SwayModifierFromJson(bad,back,&error));
    WeaponProceduralSettings definition=WeaponProceduralSettings::Defaults(), parsed;
    definition.Sway.Unity=s;
    CHECK(WeaponProceduralSettings::FromJson(definition.ToJson(),parsed,&error));
    CHECK(parsed.Sway.UnityPort && parsed.Sway.Unity.MovementSway.Space==SwaySpace::WorldSpace);
    CHECK(definition.ToJson()["sway"].contains("aimingSway") && !definition.ToJson()["sway"].contains("lookSmoothing"));
    FirstPersonAnimationSet weapon, loaded;
    weapon.ArmsModel="a.fbx"; weapon.WeaponModel="w.fbx"; weapon.Controller="c.controller";
    weapon.Procedural.Sway.Unity.AimingSway.Position.Stiffness.x=1.234567e-8f;
    CHECK(FirstPersonAnimationSet::FromJsonString(weapon.ToJsonString(),loaded,&error));
    CHECK(loaded.Procedural.Sway.Unity.AimingSway.Position.Stiffness.x==weapon.Procedural.Sway.Unity.AimingSway.Position.Stiffness.x);
}
void Springs() {
    SwayFloatSpringState state;
    const float first=SwayFloatSpringInterp(0,2,4,0.5f,0.25f,3,state,0.125f);
    CHECK(std::abs(first-1.875f)<1e-6f && state.Error==6);
    const float second=SwayFloatSpringInterp(first,1,4,0.5f,0.25f,3,state,0.125f);
    CHECK(std::abs(second-2.6015625f)<1e-6f);
    const auto saved=state;
    CHECK(SwayFloatSpringInterp(second,100,0,0.5f,0.25f,3,state,1)==second);
    CHECK(state.Error==saved.Error && state.Velocity==saved.Velocity);
    state={};
    CHECK(SwayFloatSpringInterp(0,2,100,0,1,1,state,1)==2); // min(dt * speed, 1)
    SwayModifierSettings s;
    const float dt=std::log(2.0f)/8;
    const auto simple=[&](SwayVectorSpring& v) {
        v.Damping=glm::vec3(0); v.Stiffness=glm::vec3(1); v.Speed=glm::vec3(1/dt);
        v.Scale=glm::vec3(1); v.Clamp=glm::vec3(10);
    };
    simple(s.AimingSway.Position); simple(s.AimingSway.Rotation);
    simple(s.MovementSway.Position); simple(s.MovementSway.Rotation);
    SwayModifierState job;
    auto p=job.Update(s,{1,-0.5f},{100,200},false,dt);
    CHECK(glm::length(p.Movement.Position-glm::vec3(0.005f,-0.0025f,-0.0025f))<1e-6f);
    CHECK(Same(p.Movement.Rotation,Euler({-0.25f,0.5f,0.5f})));
    CHECK(glm::length(p.Aim.Position-glm::vec3(0.005f,0.01f,0))<1e-6f);
    CHECK(Same(p.Aim.Rotation,Euler({1,0.5f,0.5f})));
    // Source clamps targets before Scale, including the movement preset's zero clamps.
    job.Reset(); s.MovementSway.Position.Clamp=s.MovementSway.Rotation.Clamp=glm::vec3(0);
    p=job.Update(s,{1,1},{0,0},false,dt);
    CHECK(p.Movement.Position==glm::vec3(0) && Same(p.Movement.Rotation,glm::quat(1,0,0,0)));
    job.Reset(); s.AimingSway.Position.Clamp=glm::vec3(0.001f); s.AimingSway.Position.Scale=glm::vec3(-2);
    p=job.Update(s,{0,0},{100,200},false,dt);
    CHECK(glm::length(p.Aim.Position-glm::vec3(-0.002f,-0.002f,0))<1e-6f);
    job.Reset(); s.AdsCurveScale=0.2f; s.AdsCurveSmoothing=8;
    p=job.Update(s,{0,0},{100,200},true,dt);
    CHECK(p.Aim.Position==glm::vec3(0)); // preset ADS scale defaults to zero
    CHECK(std::abs(p.CurveWeight-0.6f)<1e-6f);
    const auto held=job.Update(s,{1,1},{100,200},true,dt,0);
    CHECK(held.CurveWeight==p.CurveWeight && held.Aim.Position==p.Aim.Position);
    const auto invalid=job.Update(s,{1,1},{100,200},true,dt,1,false);
    CHECK(invalid.CurveWeight==held.CurveWeight);
    s.AdsCurveSmoothing=0;
    CHECK(job.Update(s,{0,0},{0,0},true,0).CurveWeight==0.2f);
    job.Reset();
    CHECK(job.Update(s,{0,0},{0,0},false,0,0).CurveWeight==1);
}
void Spaces() {
    const glm::quat identity(1,0,0,0);
    SwayModifierSettings s;
    SwayModifierPose result;
    result.Movement.Position={0.1f,0.2f,0.3f};
    result.Movement.Rotation=Euler({10,20,30});
    result.Aim.Rotation=Euler({5,7,11});
    const auto mirror=[](glm::quat q) { return glm::quat(q.w,-q.x,-q.y,q.z); };
    std::vector<LocalTRS> pose(3);
    const std::vector<int> parents={-1,0,0};
    pose[0].R=Euler({0,90,0}); pose[1].T={1,2,3};
    const auto original=pose;
    std::vector<glm::mat4> globals;
    IK::ComputeGlobals(pose,parents,globals);
    const glm::vec3 initial=IK::Position(globals[1]);
    const glm::quat initialR=IK::Rotation(globals[1]);
    const glm::vec3 translation(0.1f,0.2f,-0.3f);
    ApplySwayModifier(s,result,pose,parents,globals,1,-1,identity,identity,1,1);
    CHECK(glm::length(IK::Position(globals[1])-initial-translation)<1e-5f);
    CHECK(Same(IK::Rotation(globals[1]),mirror(result.Aim.Rotation)*mirror(result.Movement.Rotation)*initialR));
    pose=original; IK::ComputeGlobals(pose,parents,globals);
    s.MovementSway.Space=s.AimingSway.Space=SwaySpace::BoneSpace;
    ApplySwayModifier(s,result,pose,parents,globals,1,-1,identity,identity,1,1);
    CHECK(glm::length(IK::Position(globals[1])-initial-initialR*translation)<1e-5f);
    CHECK(Same(IK::Rotation(globals[1]),initialR*mirror(result.Movement.Rotation)*mirror(result.Aim.Rotation)));
    pose=original; IK::ComputeGlobals(pose,parents,globals);
    s.MovementSway.Space=s.AimingSway.Space=SwaySpace::ParentBoneSpace;
    ApplySwayModifier(s,result,pose,parents,globals,1,-1,identity,identity,1,1);
    CHECK(glm::length(pose[1].T-original[1].T-translation)<1e-5f);
    CHECK(Same(pose[1].R,mirror(result.Movement.Rotation)*mirror(result.Aim.Rotation)));
    pose=original; IK::ComputeGlobals(pose,parents,globals);
    s.MovementSway.Space=s.AimingSway.Space=SwaySpace::WorldSpace;
    const glm::quat world=Euler({0,60,0});
    ApplySwayModifier(s,result,pose,parents,globals,1,-1,identity,world,1,1);
    CHECK(glm::length(IK::Position(globals[1])-initial-glm::inverse(world)*translation)<1e-5f);
    CHECK(Same(IK::Rotation(globals[1]),initialR*mirror(result.Movement.Rotation)*mirror(result.Aim.Rotation)));
    // Curve animation reads LOCAL pose, remaps it, and applies after both springs.
    pose=original; pose[2].T={0.2f,0,0}; pose[2].R=Euler({0,0,20}); IK::ComputeGlobals(pose,parents,globals);
    s=SwayModifierSettings{}; s.SpaceOffset=Euler({0,0,90});
    result={}; result.CurveWeight=0.5f;
    ApplySwayModifier(s,result,pose,parents,globals,1,2,identity,identity,1,1);
    CHECK(glm::length(IK::Position(globals[1])-initial-glm::vec3(0,0.1f,0))<1e-5f);
    CHECK(Same(IK::Rotation(globals[1]),Euler({0,0,10})*initialR));
    CHECK(pose[2].T==glm::vec3(0.2f,0,0));
}
}
void RegisterSwayPortTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"SwayModifierSettings source presets and weapon definition",Settings});
    tests.push_back({"SwayModifierJob spring formula and target ordering",Springs});
    tests.push_back({"SwayModifierJob spaces and additive-bone animation",Spaces});
}
