#include "RecoilAnimation.h"
#include "FirstPersonProcedural.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/gtx/euler_angles.hpp>

using json=nlohmann::json;
namespace {
template<class V> json Vec(const V& v) { json j=json::array(); for (int i=0;i<V::length();++i) j.push_back(v[i]); return j; }
json CurveJson(const RecoilVectorCurve& c) {
    const auto keys=[](const Curve& curve) {
        json result=json::array();
        for (const auto& k:curve.Keys) {
            json key={k.Time,k.Value,k.InTangent,k.OutTangent};
            if(k.Interpolation!=CurveInterpolation::Cubic) key.push_back((int)k.Interpolation);
            result.push_back(std::move(key));
        }
        return result;
    };
    return {{"x",keys(c.X)},{"y",keys(c.Y)},{"z",keys(c.Z)}};
}
float Alpha(float rate,float dt) { return std::clamp(1-std::exp(-rate*dt),0.0f,1.0f); }
float Pick(std::mt19937& rng,glm::vec2 r) { return r.x==r.y?r.x:std::uniform_real_distribution<float>(std::min(r.x,r.y),std::max(r.x,r.y))(rng); }
bool Approx(float a,float b=0) { return std::abs(a-b)<std::max(1e-6f*std::max(std::abs(a),std::abs(b)),std::numeric_limits<float>::denorm_min()*8); }
float Compensation(float recoil,float compensation) {
    if (!Approx(compensation) && recoil*compensation<=0 && !Approx(recoil))
        return 1-std::clamp(std::abs(compensation/recoil),0.0f,1.0f);
    return 1;
}
glm::quat Euler(glm::vec3 degrees) {
    return glm::quat_cast(glm::yawPitchRoll(glm::radians(degrees.y),glm::radians(degrees.x),glm::radians(degrees.z)));
}
}
float RecoilVectorCurve::Length() const { return std::max({X.EndTime(),Y.EndTime(),Z.EndTime()}); }
json RecoilAnimData::ToJson() const {
    const auto progress=[](const RecoilProgression& p) { return json{{"acceleration",p.Acceleration},{"damping",p.Damping},{"amount",p.Amount}}; };
    return {{"pitch",Vec(Pitch)},{"roll",Vec(Roll)},{"yaw",Vec(Yaw)},
        {"kickback",Vec(Kickback)},{"kickUp",Vec(KickUp)},{"kickRight",Vec(KickRight)},
        {"aimRot",Vec(AimRot)},{"aimLoc",Vec(AimLoc)},{"smoothRot",Vec(SmoothRot)},{"smoothLoc",Vec(SmoothLoc)},
        {"extraRot",Vec(ExtraRot)},{"extraLoc",Vec(ExtraLoc)},
        {"noiseX",Vec(NoiseX)},{"noiseY",Vec(NoiseY)},{"noiseAccel",Vec(NoiseAccel)},{"noiseDamp",Vec(NoiseDamp)},
        {"noiseScalar",NoiseScalar},{"pushAmount",PushAmount},{"pushAccel",PushAccel},{"pushDamp",PushDamp},
        {"recoilSway",{{"pitchSway",Vec(Sway.PitchSway)},{"yawSway",Vec(Sway.YawSway)},
            {"rollMultiplier",Sway.RollMultiplier},{"damping",Sway.Damping},{"acceleration",Sway.Acceleration},
            {"adsScale",Sway.AdsScale},{"pivotOffset",Vec(Sway.PivotOffset)}}},
        {"pitchProgress",progress(PitchProgress)},{"upProgress",progress(UpProgress)},{"adsProgressAlpha",AdsProgressAlpha},
        {"horizontalRecoil",Vec(HorizontalRecoil)},{"verticalRecoil",Vec(VerticalRecoil)},
        {"horizontalSmoothing",HorizontalSmoothing},{"verticalSmoothing",VerticalSmoothing},{"damping",Damping},
        {"hipPivotOffset",Vec(HipPivotOffset)},{"aimPivotOffset",Vec(AimPivotOffset)},
        {"smoothRoll",SmoothRoll},{"playRate",PlayRate},
        {"recoilCurves",{{"semiRotCurve",CurveJson(SemiRotCurve)},{"semiLocCurve",CurveJson(SemiLocCurve)},
            {"autoRotCurve",CurveJson(AutoRotCurve)},{"autoLocCurve",CurveJson(AutoLocCurve)}}}};
}
bool RecoilAnimData::FromJson(const json& j,RecoilAnimData& out,std::string* error) {
    // Validate against the exact asset topology before conversion; failure never mutates out.
    RecoilAnimData parsed;
    json schema=parsed.ToJson();
    const auto validate=[&](const auto& self,const json& value,const json& expected,const std::string& path)->bool {
        if (expected.is_object()) {
            if (!value.is_object() || value.size()!=expected.size()) { if (error) *error=path+": expected RecoilAnimData fields"; return false; }
            for (auto it=expected.begin();it!=expected.end();++it)
                if (!value.contains(it.key()) || !self(self,value[it.key()],it.value(),path+"."+it.key())) return false;
        } else if (expected.is_array()) {
            if (!value.is_array()) return false;
            if (path.find("Curve.")!=std::string::npos) {
                Curve c; if (!Curve::FromJson(value,c) || c.Empty()) return false;
            } else {
                if (value.size()!=expected.size()) return false;
                for (const auto& v:value) if (!v.is_number() || !std::isfinite(v.get<float>())) return false;
            }
        } else if (expected.is_boolean()) { if (!value.is_boolean()) return false; }
        else if (!value.is_number() || !std::isfinite(value.get<float>())) return false;
        return true;
    };
    if (!validate(validate,j,schema,"recoil")) { if (error && error->empty()) *error="Invalid RecoilAnimData value or curve"; return false; }
    const auto vector=[&](const char* key,auto& v) { for (int i=0;i<v.length();++i) v[i]=j[key][i].template get<float>(); };
    vector("pitch",parsed.Pitch); vector("roll",parsed.Roll); vector("yaw",parsed.Yaw);
    vector("kickback",parsed.Kickback); vector("kickUp",parsed.KickUp); vector("kickRight",parsed.KickRight);
    vector("aimRot",parsed.AimRot); vector("aimLoc",parsed.AimLoc); vector("smoothRot",parsed.SmoothRot); vector("smoothLoc",parsed.SmoothLoc);
    vector("extraRot",parsed.ExtraRot); vector("extraLoc",parsed.ExtraLoc);
    vector("noiseX",parsed.NoiseX); vector("noiseY",parsed.NoiseY); vector("noiseAccel",parsed.NoiseAccel); vector("noiseDamp",parsed.NoiseDamp);
    vector("horizontalRecoil",parsed.HorizontalRecoil); vector("verticalRecoil",parsed.VerticalRecoil);
    vector("hipPivotOffset",parsed.HipPivotOffset); vector("aimPivotOffset",parsed.AimPivotOffset);
    parsed.NoiseScalar=j["noiseScalar"]; parsed.PushAmount=j["pushAmount"]; parsed.PushAccel=j["pushAccel"]; parsed.PushDamp=j["pushDamp"];
    parsed.AdsProgressAlpha=j["adsProgressAlpha"]; parsed.HorizontalSmoothing=j["horizontalSmoothing"];
    parsed.VerticalSmoothing=j["verticalSmoothing"]; parsed.Damping=j["damping"]; parsed.SmoothRoll=j["smoothRoll"]; parsed.PlayRate=j["playRate"];
    const auto progress=[](const json& p,RecoilProgression& v) { v={p["acceleration"],p["damping"],p["amount"]}; };
    progress(j["pitchProgress"],parsed.PitchProgress); progress(j["upProgress"],parsed.UpProgress);
    const auto& sway=j["recoilSway"];
    for (int i=0;i<2;++i) { parsed.Sway.PitchSway[i]=sway["pitchSway"][i]; parsed.Sway.YawSway[i]=sway["yawSway"][i]; }
    for (int i=0;i<3;++i) parsed.Sway.PivotOffset[i]=sway["pivotOffset"][i];
    parsed.Sway.RollMultiplier=sway["rollMultiplier"]; parsed.Sway.Damping=sway["damping"];
    parsed.Sway.Acceleration=sway["acceleration"]; parsed.Sway.AdsScale=sway["adsScale"];
    const auto curve=[](const json& c,RecoilVectorCurve& v) { Curve::FromJson(c["x"],v.X); Curve::FromJson(c["y"],v.Y); Curve::FromJson(c["z"],v.Z); };
    const auto& curves=j["recoilCurves"];
    curve(curves["semiRotCurve"],parsed.SemiRotCurve); curve(curves["semiLocCurve"],parsed.SemiLocCurve);
    curve(curves["autoRotCurve"],parsed.AutoRotCurve); curve(curves["autoLocCurve"],parsed.AutoLocCurve);
    if (std::max(parsed.SemiRotCurve.Length(),parsed.SemiLocCurve.Length())<0 ||
        std::max(parsed.AutoRotCurve.Length(),parsed.AutoLocCurve.Length())<0 ||
        parsed.PlayRate<0 || parsed.HorizontalSmoothing<0 || parsed.VerticalSmoothing<0 || parsed.Damping<0 ||
        parsed.NoiseScalar<0 || parsed.NoiseScalar>1 || parsed.AdsProgressAlpha<0 || parsed.AdsProgressAlpha>1 || parsed.Sway.AdsScale<0 || parsed.Sway.AdsScale>1) {
        if (error) *error="RecoilAnimData: invalid Min/Range attribute value"; return false;
    }
    out=std::move(parsed); if (error) error->clear(); return true;
}
RecoilAnimData RecoilAnimData::FromLegacy(const WeaponRecoilSettings& r) {
    RecoilAnimData d;
    const auto scaled=[](Curve c,float duration,float sign) {
        if (c.Empty()) return Curve::Line(0,0,duration,0);
        for (auto& k:c.Keys) { k.Time*=duration; k.Value*=sign; k.InTangent*=sign/duration; k.OutTangent*=sign/duration; }
        return c;
    };
    const auto curves=[&](const FirstPersonCurve3& c,float duration,glm::vec3 signs) {
        return RecoilVectorCurve{scaled(c.X,duration,signs.x),scaled(c.Y,duration,signs.y),scaled(c.Z,duration,signs.z)};
    };
    const auto peak=[](const Curve& c) { float p=0; for (const auto& k:c.Keys) if (std::abs(k.Value)>std::abs(p)) p=k.Value; return p; };
    const auto normalize=[](RecoilVectorCurve& c,glm::vec3 p) {
        Curve* axes[]={&c.X,&c.Y,&c.Z};
        for (int i=0;i<3;++i) if (std::abs(p[i])>1e-6f) for (auto& k:axes[i]->Keys) { k.Value/=p[i]; k.InTangent/=p[i]; k.OutTangent/=p[i]; }
    };
    glm::vec3 rot(-peak(r.Rotation.X),-peak(r.Rotation.Y),peak(r.Rotation.Z));
    glm::vec3 loc(peak(r.Position.X),peak(r.Position.Y),-peak(r.Position.Z));
    d.SemiRotCurve=curves(r.Rotation,r.Duration,{-1,-1,1}); d.SemiLocCurve=curves(r.Position,r.Duration,{1,1,-1});
    normalize(d.SemiRotCurve,rot); normalize(d.SemiLocCurve,loc);
    const auto range=[](glm::vec2 v,float scale) { v*=scale; return glm::vec2(std::min(v.x,v.y),std::max(v.x,v.y)); };
    d.Pitch=range(r.PitchRange,rot.x*r.HipScale);
    auto yaw=range(r.YawRange,rot.y*r.HipScale),roll=range(r.RollRange,rot.z*r.HipScale);
    const auto split=[](glm::vec2 v) { return v.x<=0 && v.y>=0 ? glm::vec4(v.x,0,0,v.y) : glm::vec4(v.x,v.y,v.x,v.y); };
    d.Yaw=split(yaw); d.Roll=split(roll);
    d.KickRight=range(r.SideRange,loc.x*r.HipScale); d.KickUp=range(r.UpRange,loc.y*r.HipScale); d.Kickback=range(r.KickRange,loc.z*r.HipScale);
    float aim=r.HipScale>1e-6f?r.AdsScale/r.HipScale:1;
    d.AimRot=r.AdsRotationScale*aim; d.AimLoc=r.AdsPositionScale*aim;
    d.AutoRotCurve=r.SustainedCurves?curves(r.SustainedRotation,r.SustainedDuration,{-1,-1,1}):d.SemiRotCurve;
    d.AutoLocCurve=r.SustainedCurves?curves(r.SustainedPosition,r.SustainedDuration,{1,1,-1}):d.SemiLocCurve;
    if (r.SustainedCurves) { normalize(d.AutoRotCurve,rot); normalize(d.AutoLocCurve,loc); }
    d.SmoothRot=d.SmoothLoc=glm::vec3(r.Smoothing.Frequency); d.ExtraRot=d.ExtraLoc=glm::vec3(1);
    d.NoiseX=r.NoiseSide; d.NoiseY=r.NoiseUp; d.NoiseAccel=glm::vec2(r.NoiseAcceleration); d.NoiseDamp=glm::vec2(r.NoiseDamping); d.NoiseScalar=std::clamp(r.NoiseAdsScale,0.0f,1.0f);
    d.PushAmount=-r.PushAmount; d.PushAccel=r.PushAcceleration; d.PushDamp=r.PushDamping;
    d.Sway.PitchSway=range(r.SwayPitch,-1); d.Sway.YawSway=range(r.SwayYaw,-1); d.Sway.RollMultiplier=-r.SwayRoll;
    d.Sway.Acceleration=r.SwayAcceleration; d.Sway.Damping=r.SwayDamping; d.Sway.AdsScale=std::clamp(r.SwayAdsScale,0.0f,1.0f);
    d.Sway.PivotOffset=r.SwayPivot*glm::vec3(1,1,-1);
    d.PitchProgress={r.ProgressionAcceleration,r.ProgressionDamping,-r.PitchProgression};
    d.UpProgress={r.ProgressionAcceleration,r.ProgressionDamping,r.UpProgression}; d.AdsProgressAlpha=std::clamp(r.ProgressionAdsScale,0.0f,1.0f);
    d.HorizontalRecoil=r.AimYaw; d.VerticalRecoil=r.AimPitch;
    d.HorizontalSmoothing=r.AimSmoothing.y; d.VerticalSmoothing=r.AimSmoothing.x; d.Damping=r.AimRecoverySpeed;
    d.HipPivotOffset=r.Pivot*glm::vec3(1,1,-1); d.AimPivotOffset=r.AdsPivot*glm::vec3(1,1,-1); d.PlayRate=1;
    return d;
}

void RecoilAnimation::SetupTransition(glm::vec3 rot,glm::vec3 loc,const RecoilVectorCurve& rc,const RecoilVectorCurve& lc) {
    m_StartRot=rot; m_StartLoc=loc; m_RestRot=m_RestLoc=glm::bvec3(true);
    m_RotCurve=rc; m_LocCurve=lc; m_LastFrameTime=std::max(rc.Length(),lc.Length());
    m_Playback=0; m_IsPlaying=true;
}
void RecoilAnimation::Play(const RecoilAnimData& d,bool aiming,float rpm,RecoilFireMode mode,std::mt19937& rng,double unscaledTime,float frameDt) {
    (void)aiming; // Targets are sampled on the solver's first frame, as in Unity Update().
    if (!m_IsFiring) m_Compensation=RecoilDelta=glm::vec2(0);
    m_IsFiring=true;
    m_TargetRecoil.x+=Pick(rng,d.HorizontalRecoil); m_TargetRecoil.y+=Pick(rng,d.VerticalRecoil);
    if (m_EnableSmoothing && !m_IsLooping) m_EnableSmoothing=false;
    const float correction=60/std::max(rpm,0.001f);
    const double elapsed=unscaledTime>=0?unscaledTime-m_LastShotTime:m_SinceShot;
    const float delta=frameDt>=0?frameDt:m_Dt;
    const float timerError=delta>0?(correction/delta+1)*delta:std::numeric_limits<float>::quiet_NaN();
    if ((elapsed>timerError+0.01f && !m_IsLooping) || mode==RecoilFireMode::Semi) {
        m_StateIndex=0; SetupTransition(m_SmoothRot,m_SmoothLoc,d.SemiRotCurve,d.SemiLocCurve);
    } else {
        m_StateIndex=1;
        if (!m_IsLooping) {
            m_EnableSmoothing=true;
            glm::vec3 alpha=d.AutoRotCurve.Evaluate(correction);
            m_StartRot+= (m_TargetRot-m_StartRot)*alpha;
            alpha=d.AutoLocCurve.Evaluate(correction); m_StartLoc+=(m_TargetLoc-m_StartLoc)*alpha;
            SetupTransition(m_StartRot,m_StartLoc,d.AutoRotCurve,d.AutoLocCurve);
            m_PushTarget=d.PushAmount; m_LastFrameTime=correction; m_IsLooping=true;
        }
    }
    m_SinceShot=0;
    if (unscaledTime>=0) m_LastShotTime=unscaledTime;
}
void RecoilAnimation::Stop() {
    if (!m_IsFiring) return; // host may request Stop every idle frame; Unity invokes it on release
    m_IsFiring=false;
    for (int i=0;i<2;++i) m_Recoil[i]*=Compensation(m_Recoil[i],m_Compensation[i]);
    m_CachedRecoil=m_TargetRecoil=m_Recoil;
    if (m_StateIndex==1 && m_IsLooping) { m_LastFrameTime=std::max(m_RotCurve.Length(),m_LocCurve.Length()); m_IsPlaying=true; }
    m_IsLooping=false;
}
void RecoilAnimation::CalculateTargets(const RecoilAnimData& d,bool aiming,std::mt19937& rng) {
    float pitch=Pick(rng,d.Pitch);
    float yawMin=Pick(rng,{d.Yaw.x,d.Yaw.y}),yawMax=Pick(rng,{d.Yaw.z,d.Yaw.w});
    float yaw=Pick(rng,{0,1})>=0.5f?yawMax:yawMin;
    float rollMin=Pick(rng,{d.Roll.x,d.Roll.y}),rollMax=Pick(rng,{d.Roll.z,d.Roll.w});
    float roll=Pick(rng,{0,1})>=0.5f?rollMax:rollMin;
    if (m_TargetRot.z*roll>0 && d.SmoothRoll) roll=-roll;
    float kick=Pick(rng,d.Kickback),right=Pick(rng,d.KickRight),up=Pick(rng,d.KickUp);
    m_NoiseTarget.x+=Pick(rng,d.NoiseX); m_NoiseTarget.y+=Pick(rng,d.NoiseY);
    if (aiming) m_NoiseTarget*=d.NoiseScalar;
    m_TargetRot=glm::vec3(pitch,yaw,roll)*(aiming?d.AimRot:glm::vec3(1));
    m_TargetLoc=glm::vec3(right,up,kick)*(aiming?d.AimLoc:glm::vec3(1));
    float scalar=aiming?d.AdsProgressAlpha:1;
    m_PitchProgress.y+=d.PitchProgress.Amount*scalar; m_UpProgress.y+=d.UpProgress.Amount*scalar;
    m_PitchSway.y+=Pick(rng,d.Sway.PitchSway)*(aiming?d.Sway.AdsScale:1);
    m_YawSway.y+=Pick(rng,d.Sway.YawSway)*(aiming?d.Sway.AdsScale:1);
}
void RecoilAnimation::Update(const RecoilAnimData& d,bool aiming,float dt,std::mt19937& rng) {
    m_Dt=std::max(0.0f,std::isfinite(dt)?dt:0.0f); m_SinceShot+=m_Dt;
    if (m_IsFiring) m_Compensation+=m_DeltaInput;
    m_Recoil.x+=(m_TargetRecoil.x-m_Recoil.x)*Alpha(d.HorizontalSmoothing,m_Dt);
    m_Recoil.y+=(m_TargetRecoil.y-m_Recoil.y)*Alpha(d.VerticalSmoothing,m_Dt);
    if (!m_IsFiring) m_TargetRecoil*=1-Alpha(d.Damping,m_Dt);
    RecoilDelta=m_Recoil-m_CachedRecoil; m_CachedRecoil=m_Recoil;
    if (m_IsPlaying) {
        // Unity VectorCurve copies retain references to the asset's AnimationCurve objects.
        // Refresh the host's value copies so curve edits also affect an active transition.
        m_RotCurve=m_StateIndex==0?d.SemiRotCurve:d.AutoRotCurve;
        m_LocCurve=m_StateIndex==0?d.SemiLocCurve:d.AutoLocCurve;
        if (Approx(m_Playback)) CalculateTargets(d,aiming,rng);
        float last=std::max(0.0f,m_Playback-m_Dt*d.PlayRate);
        const auto solve=[&](const RecoilVectorCurve& curve,glm::vec3& start,const glm::vec3& target,glm::bvec3& rest) {
            glm::vec3 alpha=curve.Evaluate(m_Playback),previous=curve.Evaluate(last),out;
            for (int i=0;i<3;++i) {
                if (std::abs(previous[i])>std::abs(alpha[i]) && rest[i] && !m_IsLooping) { start[i]=0; rest[i]=false; }
                out[i]=start[i]+(target[i]-start[i])*alpha[i];
            }
            return out;
        };
        m_RawRot=solve(m_RotCurve,m_StartRot,m_TargetRot,m_RestRot);
        m_RawLoc=solve(m_LocCurve,m_StartLoc,m_TargetLoc,m_RestLoc);
        const auto smooth=[&](glm::vec3& out,glm::vec3 raw,glm::vec3 rate,glm::vec3 extra) {
            for (int i=0;i<3;++i) {
                float scale=Approx(extra[i])?1:extra[i];
                out[i]=Approx(rate[i])?raw[i]*scale:out[i]+(raw[i]*scale-out[i])*Alpha(rate[i],m_Dt);
            }
        };
        if (m_EnableSmoothing) { smooth(m_SmoothRot,m_RawRot,d.SmoothRot,d.ExtraRot); smooth(m_SmoothLoc,m_RawLoc,d.SmoothLoc,d.ExtraLoc); }
        else { m_SmoothRot=m_RawRot; m_SmoothLoc=m_RawLoc; }
        m_Playback=std::clamp(m_Playback+m_Dt*d.PlayRate,0.0f,m_LastFrameTime);
        if (Approx(m_Playback,m_LastFrameTime)) { m_Playback=0; m_IsPlaying=m_IsLooping; }
    }
    glm::vec3 loc=m_SmoothLoc,rot=m_SmoothRot;
    // Source ordering matters: noise/push decay before following; progression/sway follow before decay.
    for (int i=0;i<2;++i) { m_NoiseTarget[i]*=1-Alpha(d.NoiseDamp[i],m_Dt); m_NoiseOut[i]+=(m_NoiseTarget[i]-m_NoiseOut[i])*Alpha(d.NoiseAccel[i],m_Dt); }
    loc+=glm::vec3(m_NoiseOut,0);
    m_PushTarget*=1-Alpha(d.PushDamp,m_Dt); m_PushOut+=(m_PushTarget-m_PushOut)*Alpha(d.PushAccel,m_Dt); loc.z+=m_PushOut;
    const auto progress=[&](glm::vec2& state,float acceleration,float damping) { state.x+=(state.y-state.x)*Alpha(acceleration,m_Dt); state.y*=1-Alpha(damping,m_Dt); };
    progress(m_PitchProgress,d.PitchProgress.Acceleration,d.PitchProgress.Damping);
    progress(m_UpProgress,d.UpProgress.Acceleration,d.UpProgress.Damping);
    rot.x+=m_PitchProgress.x; loc.y+=m_UpProgress.x;
    glm::quat rotation=Euler(rot);
    glm::vec3 pivot=aiming?d.AimPivotOffset:d.HipPivotOffset;
    loc+=rotation*pivot-pivot;
    progress(m_PitchSway,d.Sway.Acceleration,d.Sway.Damping); progress(m_YawSway,d.Sway.Acceleration,d.Sway.Damping);
    glm::quat sway=Euler({m_PitchSway.x,m_YawSway.x,m_YawSway.x*d.Sway.RollMultiplier});
    loc+=sway*d.Sway.PivotOffset-d.Sway.PivotOffset;
    OutRot=rotation*sway; OutLoc=loc;
}
