#include "CameraEffects.h"
#include "Camera.h"
#include "AtomicFile.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <tuple>

namespace {
glm::quat RotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for(int i=0;i<3;++i) r[i]=glm::normalize(r[i]);
    return glm::normalize(glm::quat_cast(r));
}
bool Fail(std::string* error,const std::string& why) { if(error) *error=why; return false; }
bool ValidCameraCurve(const Curve& curve) {
    return (curve.Empty() || (curve.StartTime()>=0 && curve.EndTime()<=1)) &&
        std::abs(curve.Evaluate(0))<=1e-5f && std::abs(curve.Evaluate(1))<=1e-5f;
}
}
CameraEffectPose ActionCameraDelta(const glm::mat4& reference,const glm::mat4& animated,
                                  const glm::quat& modelToView,float modelScale,
                                  float rotationScale,float positionScale) {
    CameraEffectPose out;
    const glm::quat delta=RotationOf(animated)*glm::inverse(RotationOf(reference));
    out.Rotation=glm::normalize(glm::slerp(glm::quat(1,0,0,0),
        modelToView*delta*glm::inverse(modelToView),rotationScale));
    out.Position=modelToView*(glm::vec3(animated[3])-glm::vec3(reference[3]))*modelScale*positionScale;
    return out;
}
void ApplyCameraEffect(Camera& camera,const CameraEffectPose& effect) {
    glm::mat3 basis(camera.Right(),camera.Up(),-camera.Front());
    camera.Position+=basis*effect.Position;
    const glm::mat3 posed=basis*glm::mat3_cast(effect.Rotation);
    const glm::vec3 forward=-posed[2];
    camera.Pitch=std::clamp(glm::degrees(std::asin(std::clamp(forward.y,-1.0f,1.0f))),-89.0f,89.0f);
    const float yaw=glm::degrees(std::atan2(forward.z,forward.x));
    camera.Yaw+=std::remainder(yaw-camera.Yaw,360.0f);
    const glm::vec3 right=glm::normalize(glm::cross(camera.Front(),glm::vec3(0,1,0)));
    const glm::vec3 up=glm::normalize(glm::cross(right,camera.Front()));
    camera.Roll=glm::degrees(std::atan2(glm::dot(posed[0],up),glm::dot(posed[0],right)));
}
std::string CameraShakeAsset::ToJsonString() const {
    auto curves=[](const Curve (&c)[3]) { return nlohmann::json::array({c[0].ToJson(),c[1].ToJson(),c[2].ToJson()}); };
    const auto vector=[](const glm::vec3& v){return nlohmann::json::array({v.x,v.y,v.z});};
    return nlohmann::json{{"version",2},{"duration",Duration},{"adsScale",AdsScale},
        {"rotationScalarMin",vector(RotationScalarMin)},{"rotationScalarMax",vector(RotationScalarMax)},
        {"locationScalarMin",vector(LocationScalarMin)},{"locationScalarMax",vector(LocationScalarMax)},
        {"rotationCurves",curves(Rotation)},{"positionCurves",curves(Position)}}.dump(2)+"\n";
}
bool CameraShakeAsset::FromJsonString(const std::string& text,CameraShakeAsset& out,std::string* error) {
    try {
        const auto j=nlohmann::json::parse(text);
        if(!j.is_object()) return Fail(error,"camera shake must be an object");
        const int version=j.value("version",1);
        if(version!=1 && version!=2) return Fail(error,"unsupported camera shake version");
        CameraShakeAsset s;
        auto number=[&](const char* key,float& v,float lo,float hi) {
            if(j.contains(key)) v=j.at(key).get<float>();
            return std::isfinite(v) && v>=lo && v<=hi;
        };
        if(!number("duration",s.Duration,.001f,10) || !number("adsScale",s.AdsScale,0,2))
            return Fail(error,"invalid camera shake duration or ADS scale");
        // Previously authored shared ranges seed every axis; explicit Vector3 ranges take priority.
        float legacyMin=1,legacyMax=1;
        if(j.contains("scalarMin"))legacyMin=j.at("scalarMin").get<float>();
        if(j.contains("scalarMax"))legacyMax=j.at("scalarMax").get<float>();
        if(!std::isfinite(legacyMin)||!std::isfinite(legacyMax)||legacyMin>legacyMax)
            return Fail(error,"invalid legacy camera shake scalar range");
        s.RotationScalarMin=s.LocationScalarMin=glm::vec3(legacyMin);
        s.RotationScalarMax=s.LocationScalarMax=glm::vec3(legacyMax);
        const auto vector=[&](const char* key,glm::vec3& v) {
            if(!j.contains(key))return true;
            const auto& a=j.at(key);
            if(!a.is_array()||a.size()!=3)return false;
            for(int i=0;i<3;++i){if(!a[i].is_number())return false;v[i]=a[i].get<float>();if(!std::isfinite(v[i]))return false;}
            return true;
        };
        if(!vector("rotationScalarMin",s.RotationScalarMin)||!vector("rotationScalarMax",s.RotationScalarMax)||
           !vector("locationScalarMin",s.LocationScalarMin)||!vector("locationScalarMax",s.LocationScalarMax))
            return Fail(error,"camera shake scalar ranges need three finite components");
        for(int i=0;i<3;++i)if(s.RotationScalarMin[i]>s.RotationScalarMax[i]||s.LocationScalarMin[i]>s.LocationScalarMax[i])
            return Fail(error,"camera shake scalar min must be <= max on every axis");
        if(version==1) {
            // Preserve the authored envelope and amplitudes, baking them into each axis.
            // The old frequency is intentionally ignored: playback has no noise stage.
            Curve envelope=Curve::Kick(.08f);
            if(j.contains("envelope") && !Curve::FromJson(j.at("envelope"),envelope)) return Fail(error,"invalid shake envelope");
            if(!ValidCameraCurve(envelope)) return Fail(error,"shake envelope must start/end at zero within 0..1");
            for(auto [key,dst,amplitude,limit]:{
                std::tuple{"rotation",s.Rotation,glm::vec3(.3f,.18f,.35f),90.0f},
                std::tuple{"position",s.Position,glm::vec3(.0008f,.0008f,.0012f),1.0f}}) {
                if(j.contains(key)) {
                    const auto& a=j.at(key);
                    if(!a.is_array() || a.size()!=3) return Fail(error,std::string(key)+" needs three amplitudes");
                    for(int i=0;i<3;++i) {
                        amplitude[i]=a[i].get<float>();
                        if(!std::isfinite(amplitude[i]) || amplitude[i]<0 || amplitude[i]>limit)
                            return Fail(error,std::string(key)+" amplitude out of range");
                    }
                }
                for(int i=0;i<3;++i) {
                    dst[i]=envelope;
                    for(auto& k:dst[i].Keys) { k.Value*=amplitude[i]; k.InTangent*=amplitude[i]; k.OutTangent*=amplitude[i]; }
                }
            }
        } else {
            for(auto [key,dst]:{std::pair{"rotationCurves",s.Rotation},std::pair{"positionCurves",s.Position}}) {
                if(!j.contains(key)) continue;
                const auto& channels=j.at(key);
                if(!channels.is_array() || channels.size()!=3) return Fail(error,std::string(key)+" needs three curves");
                for(int i=0;i<3;++i)
                    if(!Curve::FromJson(channels[i],dst[i]) || !ValidCameraCurve(dst[i]))
                        return Fail(error,std::string(key)+" curves must start/end at zero within 0..1");
            }
        }
        out=std::move(s); if(error) error->clear(); return true;
    } catch(const std::exception& e) { return Fail(error,std::string("invalid camera shake: ")+e.what()); }
}
bool CameraShakeAsset::LoadFile(const std::string& path,CameraShakeAsset& out,std::string* error) {
    std::ifstream in(std::filesystem::u8path(path));
    if(!in) return Fail(error,"could not open camera shake '"+path+"'");
    return FromJsonString(std::string(std::istreambuf_iterator<char>(in),{}),out,error);
}
bool CameraShakeAsset::SaveFile(const std::string& path) const {
    CameraShakeAsset valid;
    const auto text=ToJsonString();
    return FromJsonString(text,valid) && AtomicFile::WriteBytes(std::filesystem::u8path(path),text);
}
void CameraShakeState::Trigger(const CameraShakeAsset& asset,bool ads) {
    if(m_Shots.size()>=32) m_Shots.erase(m_Shots.begin());
    const auto sample=[&](float a,float b) {
        const float lo=std::min(a,b),hi=std::max(a,b);
        return lo==hi?lo:float(std::uniform_real_distribution<double>(lo,hi)(m_Rng));
    };
    glm::vec3 rotationScale,locationScale;
    for(int i=0;i<3;++i) {
        rotationScale[i]=sample(asset.RotationScalarMin[i],asset.RotationScalarMax[i]);
        locationScale[i]=sample(asset.LocationScalarMin[i],asset.LocationScalarMax[i]);
    }
    const float adsScale=ads?asset.AdsScale:1.0f;
    m_Shots.push_back({asset,0,rotationScale*adsScale,locationScale*adsScale});
}
CameraEffectPose CameraShakeState::Update(float dt) {
    CameraEffectPose out;
    glm::vec3 angles(0);
    for(auto& shot:m_Shots) {
        shot.Age+=std::max(dt,0.0f);
        if(shot.Age>=shot.Asset.Duration) continue;
        const float t=shot.Age/shot.Asset.Duration;
        for(int i=0;i<3;++i) {
            angles[i]+=shot.Asset.Rotation[i].Evaluate(t)*shot.RotationScale[i];
            out.Position[i]+=shot.Asset.Position[i].Evaluate(t)*shot.LocationScale[i];
        }
    }
    m_Shots.erase(std::remove_if(m_Shots.begin(),m_Shots.end(),[](const Shot& s){return s.Age>=s.Asset.Duration;}),m_Shots.end());
    // Camera roll is positive about its forward (-Z), opposite local quaternion Z.
    out.Rotation=glm::normalize(glm::angleAxis(glm::radians(angles.y),glm::vec3(0,1,0))*
        glm::angleAxis(glm::radians(angles.x),glm::vec3(1,0,0))*glm::angleAxis(glm::radians(-angles.z),glm::vec3(0,0,1)));
    return out;
}
