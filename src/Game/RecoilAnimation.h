#pragma once
#include "Curve.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <json.hpp>
#include <random>
#include <string>

struct WeaponRecoilSettings;
struct RecoilVectorCurve {
    Curve X=Curve::Line(0,0,1,0), Y=X, Z=X;
    glm::vec3 Evaluate(float t) const { return {X.Evaluate(t),Y.Evaluate(t),Z.Evaluate(t)}; }
    float Length() const;
};
struct RecoilProgression { float Acceleration=0, Damping=0, Amount=0; };
struct RecoilSway {
    glm::vec2 PitchSway{0}, YawSway{0};
    float RollMultiplier=0, Damping=0, Acceleration=0, AdsScale=0;
    glm::vec3 PivotOffset{0};
};
// RecoilAnimData.cs fields, units and defaults. Curves are interpolation alphas keyed in seconds.
// Values remain in Unity's local axes (+Z forward); conversion happens at the engine boundary.
struct RecoilAnimData {
    glm::vec2 Pitch{0}; glm::vec4 Roll{0}, Yaw{0};
    glm::vec2 Kickback{0}, KickUp{0}, KickRight{0};
    glm::vec3 AimRot{0}, AimLoc{0}, SmoothRot{0}, SmoothLoc{0}, ExtraRot{0}, ExtraLoc{0};
    glm::vec2 NoiseX{0}, NoiseY{0}, NoiseAccel{0}, NoiseDamp{0};
    float NoiseScalar=1, PushAmount=0, PushAccel=0, PushDamp=0;
    RecoilSway Sway;
    RecoilProgression PitchProgress, UpProgress;
    float AdsProgressAlpha=1;
    glm::vec2 HorizontalRecoil{0}, VerticalRecoil{0};
    float HorizontalSmoothing=0, VerticalSmoothing=0, Damping=0;
    glm::vec3 HipPivotOffset{0}, AimPivotOffset{0};
    bool SmoothRoll=false;
    float PlayRate=0;
    RecoilVectorCurve SemiRotCurve, SemiLocCurve, AutoRotCurve, AutoLocCurve;
    nlohmann::json ToJson() const;
    static bool FromJson(const nlohmann::json&,RecoilAnimData&,std::string* error=nullptr);
    static RecoilAnimData FromLegacy(const WeaponRecoilSettings&);
};

enum class RecoilFireMode { Semi, Burst, Auto };
// RecoilAnimation.cs timeline and solver, one instance per equipped presentation.
class RecoilAnimation {
public:
    void Play(const RecoilAnimData&,bool aiming,float rpm,RecoilFireMode,std::mt19937&,
              double unscaledTime=-1,float frameDt=-1);
    void Stop();
    void Update(const RecoilAnimData&,bool aiming,float dt,std::mt19937&);
    void UpdateDeltaInput(glm::vec2 input) { m_DeltaInput=input; }
    bool IsFiring() const { return m_IsFiring; }
    bool IsPlaying() const { return m_IsPlaying; }
    bool IsLooping() const { return m_IsLooping; }
    glm::vec3 OutLoc{0};
    glm::quat OutRot{1,0,0,0};
    glm::vec2 RecoilDelta{0}; // Unity order: horizontal, vertical
private:
    void CalculateTargets(const RecoilAnimData&,bool,std::mt19937&);
    void SetupTransition(glm::vec3,glm::vec3,const RecoilVectorCurve&,const RecoilVectorCurve&);
    glm::vec3 m_TargetRot{0},m_TargetLoc{0},m_StartRot{0},m_StartLoc{0},m_RawRot{0},m_RawLoc{0},m_SmoothRot{0},m_SmoothLoc{0};
    glm::bvec3 m_RestRot{true},m_RestLoc{true};
    RecoilVectorCurve m_RotCurve,m_LocCurve;
    glm::vec2 m_NoiseTarget{0},m_NoiseOut{0},m_PitchSway{0},m_YawSway{0},m_PitchProgress{0},m_UpProgress{0};
    float m_PushTarget=0,m_PushOut=0,m_Playback=0,m_LastFrameTime=0,m_SinceShot=1e9f,m_Dt=1.0f/60;
    double m_LastShotTime=0;
    bool m_IsPlaying=false,m_IsLooping=false,m_EnableSmoothing=false,m_IsFiring=false;
    int m_StateIndex=0;
    glm::vec2 m_DeltaInput{0},m_Compensation{0},m_TargetRecoil{0},m_Recoil{0},m_CachedRecoil{0};
};
