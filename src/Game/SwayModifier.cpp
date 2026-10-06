#include "SwayModifier.h"
#include "SwayModifierSerialization.h"
#include "IK.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <glm/gtx/euler_angles.hpp>

namespace {
using json = nlohmann::json;
json Vec(glm::vec3 v) { return json::array({v.x,v.y,v.z}); }
json SpringJson(const SwayVectorSpring& s) {
    return {{"damping",Vec(s.Damping)},{"stiffness",Vec(s.Stiffness)},{"speed",Vec(s.Speed)},
            {"scale",Vec(s.Scale)},{"clamp",Vec(s.Clamp)}};
}
const char* SpaceName(SwaySpace s) {
    const char* names[] = {"BoneSpace","ParentBoneSpace","ComponentSpace","WorldSpace"};
    return names[static_cast<int>(s)];
}
json SwayJson(const SwaySpringSettings& s) {
    return {{"position",SpringJson(s.Position)},{"rotation",SpringJson(s.Rotation)},
            {"dampingFactor",s.DampingFactor},{"space",SpaceName(s.Space)},{"adsScale",s.AdsScale}};
}
bool ApproxZero(float v) { return std::abs(v) < std::max(1e-6f*std::abs(v),std::numeric_limits<float>::denorm_min()*8); }
float Alpha(float rate,float dt) { return std::clamp(1-std::exp(-rate*dt),0.0f,1.0f); }
glm::quat Euler(glm::vec3 v) {
    return glm::normalize(glm::quat_cast(glm::yawPitchRoll(glm::radians(v.y),glm::radians(v.x),glm::radians(v.z))));
}
glm::quat Mirror(glm::quat q) { return {q.w,-q.x,-q.y,q.z}; }
glm::vec3 Mirror(glm::vec3 p) { return p*glm::vec3(1,1,-1); }
glm::vec3 Spring(glm::vec3 current,glm::vec3 target,const SwayVectorSpring& spring,SwayFloatSpringState* state,float dt) {
    for (int i=0;i<3;++i) {
        // Mathf.Clamp's ordered branches also define behavior for a negative authored clamp.
        const float limit=spring.Clamp[i];
        if (target[i] < -limit) target[i]=-limit;
        else if (target[i] > limit) target[i]=limit;
        current[i]=SwayFloatSpringInterp(current[i],target[i],spring.Speed[i],spring.Damping[i],
                                         spring.Stiffness[i],spring.Scale[i],state[i],dt);
    }
    return current;
}
}
SwaySpringSettings SwaySpringSettings::ShooterAimPreset() {
    SwaySpringSettings s;
    s.Position={glm::vec3(0.4f),{0.4f,0.4f,0.8f},glm::vec3(7),glm::vec3(1),glm::vec3(1)};
    s.Rotation={{0.4f,0.4f,0.3f},glm::vec3(0.8f),{15,20,15},{-2,2,-2},glm::vec3(1)};
    s.DampingFactor=8;
    return s;
}
SwaySpringSettings SwaySpringSettings::ShooterMovePreset() {
    SwaySpringSettings s;
    s.Position={glm::vec3(0.4f),glm::vec3(0.8f),glm::vec3(7),{1,0,1},glm::vec3(0)};
    s.Rotation={glm::vec3(0.4f),glm::vec3(0.8f),glm::vec3(12),{2,2,-2},glm::vec3(0)};
    s.DampingFactor=8;
    return s;
}
json SwayModifierToJson(const SwayModifierSettings& s) {
    return {{"weaponBone",s.WeaponBone},{"weaponAdditiveBone",s.WeaponAdditiveBone},
            {"aimingSway",SwayJson(s.AimingSway)},{"movementSway",SwayJson(s.MovementSway)},
            {"spaceOffset",json::array({s.SpaceOffset.x,s.SpaceOffset.y,s.SpaceOffset.z,s.SpaceOffset.w})},
            {"adsCurveScale",s.AdsCurveScale},{"adsCurveSmoothing",s.AdsCurveSmoothing}};
}
bool SwayModifierFromJson(const json& j,SwayModifierSettings& out,std::string* error) {
    try {
        SwayModifierSettings s;
        const auto vector=[](const json& v) {
            if (!v.is_array() || v.size()!=3) throw std::runtime_error("expected Vector3");
            glm::vec3 r;
            for (int i=0;i<3;++i) {
                if (!v[i].is_number() || !std::isfinite(v[i].get<float>())) throw std::runtime_error("non-finite Vector3");
                r[i]=v[i].get<float>();
            }
            return r;
        };
        const auto number=[](const json& v) {
            if (!v.is_number() || !std::isfinite(v.get<float>())) throw std::runtime_error("expected finite number");
            return v.get<float>();
        };
        const auto spring=[&](const json& v) {
            SwayVectorSpring r{vector(v.at("damping")),vector(v.at("stiffness")),vector(v.at("speed")),vector(v.at("scale")),vector(v.at("clamp"))};
            if (glm::any(glm::lessThan(r.Stiffness,glm::vec3(0)))) throw std::runtime_error("negative spring stiffness");
            return r;
        };
        const auto sway=[&](const json& v) {
            SwaySpringSettings r;
            r.Position=spring(v.at("position")); r.Rotation=spring(v.at("rotation"));
            r.DampingFactor=number(v.at("dampingFactor")); r.AdsScale=number(v.at("adsScale"));
            if (r.AdsScale<0 || r.AdsScale>1) throw std::runtime_error("adsScale must be 0..1");
            bool found=false;
            for (int i=0;i<4;++i) if (v.at("space")==SpaceName(static_cast<SwaySpace>(i))) { r.Space=static_cast<SwaySpace>(i); found=true; }
            if (!found) throw std::runtime_error("invalid sway space");
            return r;
        };
        s.WeaponBone=j.at("weaponBone").get<std::string>();
        s.WeaponAdditiveBone=j.at("weaponAdditiveBone").get<std::string>();
        s.AimingSway=sway(j.at("aimingSway")); s.MovementSway=sway(j.at("movementSway"));
        const auto& q=j.at("spaceOffset");
        if (!q.is_array() || q.size()!=4) throw std::runtime_error("spaceOffset must be quaternion XYZW");
        s.SpaceOffset={number(q[3]),number(q[0]),number(q[1]),number(q[2])};
        if (glm::length(s.SpaceOffset)<1e-6f) throw std::runtime_error("zero spaceOffset quaternion");
        s.SpaceOffset=glm::normalize(s.SpaceOffset);
        s.AdsCurveScale=number(j.at("adsCurveScale")); s.AdsCurveSmoothing=number(j.at("adsCurveSmoothing"));
        if (s.AdsCurveScale<0 || s.AdsCurveScale>1 || s.AdsCurveSmoothing<0) throw std::runtime_error("invalid curve ADS range");
        out=std::move(s); if (error) error->clear(); return true;
    } catch (const std::exception& e) { if (error) *error=std::string("sway: ")+e.what(); return false; }
}
float SwayFloatSpringInterp(float current,float target,float speed,float criticalDamping,float stiffness,
                           float scale,SwayFloatSpringState& state,float dt) {
    const float interpSpeed=std::min(dt*speed,1.0f);
    if (!ApproxZero(interpSpeed)) {
        const float damping=2*std::sqrt(stiffness)*criticalDamping;
        const float error=target*scale-current;
        const float errorDeriv=error-state.Error;
        state.Velocity+=error*stiffness*interpSpeed+errorDeriv*damping;
        state.Error=error;
        return current+state.Velocity*interpSpeed;
    }
    return current;
}
const SwayModifierPose& SwayModifierState::Update(const SwayModifierSettings& s,glm::vec2 move,glm::vec2 look,
                                               bool aiming,float dt,float weight,bool validWeapon) {
    if (ApproxZero(weight) || !validWeapon) return m_Pose;
    const auto& m=s.MovementSway;
    const float moveScale=aiming?m.AdsScale:1;
    const glm::vec3 moveRot=glm::vec3(move.y,move.x,move.x)*moveScale;
    const glm::vec3 movePos=glm::vec3(move.x,move.y,move.y)*moveScale/100.0f;
    const float alpha=Alpha(m.DampingFactor,dt);
    m_MovePositionTarget=glm::mix(m_MovePositionTarget,movePos,alpha);
    m_MoveRotationTarget=glm::mix(m_MoveRotationTarget,moveRot,alpha);
    m_MovePosition=Spring(m_MovePosition,m_MovePositionTarget,m.Position,m_MovePosState,dt);
    m_MoveRotation=Spring(m_MoveRotation,m_MoveRotationTarget,m.Rotation,m_MoveRotState,dt);
    m_Pose.Movement={m_MovePosition,Euler(m_MoveRotation)};
    const auto& a=s.AimingSway;
    m_AimTarget+=look*0.01f;
    m_AimTarget=glm::mix(m_AimTarget,glm::vec2(0),Alpha(a.DampingFactor,dt));
    const float aimScale=aiming?a.AdsScale:1;
    const glm::vec3 aimPos=glm::vec3(m_AimTarget,0)*aimScale/100.0f;
    const glm::vec3 aimRot=glm::vec3(m_AimTarget.y,m_AimTarget.x,m_AimTarget.x)*aimScale;
    m_AimPosition=Spring(m_AimPosition,aimPos,a.Position,m_AimPosState,dt);
    m_AimRotation=Spring(m_AimRotation,aimRot,a.Rotation,m_AimRotState,dt);
    m_Pose.Aim={m_AimPosition,Euler(m_AimRotation)};
    const float curveTarget=aiming?s.AdsCurveScale:1;
    m_Pose.CurveWeight=s.AdsCurveSmoothing>0?glm::mix(m_Pose.CurveWeight,curveTarget,Alpha(s.AdsCurveSmoothing,dt)):curveTarget;
    return m_Pose;
}
void ApplySwayModifier(const SwayModifierSettings& s,const SwayModifierPose& result,
                       std::vector<LocalTRS>& pose,const std::vector<int>& parents,std::vector<glm::mat4>& globals,
                       int gun,int additive,glm::quat component,glm::quat world,float units,float weight) {
    if (gun<0 || gun>=static_cast<int>(pose.size()) || units<=0 || weight<=0) return;
    const float w=std::clamp(weight,0.0f,1.0f);
    const glm::quat ci=glm::inverse(component);
    const auto apply=[&](const SwayTransform& t,SwaySpace space) {
        const glm::vec3 p=Mirror(t.Position)/units;
        const glm::quat q=Mirror(t.Rotation);
        if (space==SwaySpace::ParentBoneSpace) {
            pose[gun].T+=p*w;
            pose[gun].R=glm::normalize(glm::slerp(pose[gun].R,pose[gun].R*q,w));
            IK::ComputeGlobals(pose,parents,globals);
            return;
        }
        const glm::quat gr=IK::Rotation(globals[gun]);
        glm::vec3 translation;
        glm::quat rotation;
        if (space==SwaySpace::BoneSpace) { translation=gr*p; rotation=gr*q*glm::inverse(gr); }
        else if (space==SwaySpace::ComponentSpace) { translation=ci*p; rotation=ci*q*component; }
        else {
            translation=glm::inverse(world)*p;
            // The source WorldSpace branch postmultiplies the target's world rotation.
            rotation=gr*q*glm::inverse(gr);
        }
        IK::OffsetBone(pose,parents,globals,gun,translation*w,glm::slerp(glm::quat(1,0,0,0),rotation,w),IK::Position(globals[gun]));
    };
    apply(result.Movement,s.MovementSway.Space);
    apply(result.Aim,s.AimingSway.Space);
    if (additive>=0 && additive<static_cast<int>(pose.size())) {
        const glm::quat offset=ci*Mirror(s.SpaceOffset)*component;
        const glm::vec3 p=offset*pose[additive].T;
        const glm::quat r=glm::normalize(offset*pose[additive].R*glm::inverse(offset));
        const float curveWeight=std::clamp(w*result.CurveWeight,0.0f,1.0f);
        IK::OffsetBone(pose,parents,globals,gun,p*curveWeight,glm::slerp(glm::quat(1,0,0,0),r,curveWeight),IK::Position(globals[gun]));
    }
}
