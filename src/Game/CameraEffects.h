#pragma once
#include "Curve.h"
#include <glm/gtc/quaternion.hpp>
#include <random>
#include <string>
#include <vector>

class Camera;
struct CameraEffectPose {
    glm::vec3 Position{0}; // camera-local metres: right, up, back
    glm::quat Rotation{1,0,0,0};
};
struct ActionCameraSettings {
    bool Enabled=false;
    std::string Node; // baked camera bone on the arms rig; empty falls back to the scene camera bone
    std::string ReferenceState="Idle";
    std::vector<std::string> Tags{"Reload","Busy","Cycling"};
    std::vector<std::string> States; // additional states, e.g. Draw
    float RotationScale=1, PositionScale=0, AdsScale=1;
};
// Authored node motion relative to the neutral reference, expressed in the view frame.
CameraEffectPose ActionCameraDelta(const glm::mat4& reference,const glm::mat4& animated,
                                  const glm::quat& modelToView,float modelScale,
                                  float rotationScale,float positionScale);
void ApplyCameraEffect(Camera& camera,const CameraEffectPose& effect);

struct CameraShakeAsset {
    float Duration=.18f, AdsScale=.55f;
    glm::vec3 RotationScalarMin{1}, RotationScalarMax{1}; // X pitch, Y yaw, Z roll
    glm::vec3 LocationScalarMin{1}, LocationScalarMax{1}; // X right, Y up, Z back
    // Direct, signed offsets over normalized shot time 0..1, scaled independently per axis/shot.
    Curve Rotation[3] = {Curve::Line(0,0,1,0),Curve::Line(0,0,1,0),Curve::Line(0,0,1,0)}; // pitch/yaw/roll degrees
    Curve Position[3] = {Curve::Line(0,0,1,0),Curve::Line(0,0,1,0),Curve::Line(0,0,1,0)}; // right/up/back metres
    static bool FromJsonString(const std::string& text,CameraShakeAsset& out,std::string* error=nullptr);
    static bool LoadFile(const std::string& path,CameraShakeAsset& out,std::string* error=nullptr);
    std::string ToJsonString() const;
    bool SaveFile(const std::string& path) const;
};
class CameraShakeState {
public:
    explicit CameraShakeState(unsigned seed=std::random_device{}()) : m_Rng(seed) {}
    void Reset() { m_Shots.clear(); }
    void Trigger(const CameraShakeAsset& asset,bool ads);
    CameraEffectPose Update(float dt);
private:
    struct Shot {
        CameraShakeAsset Asset;
        float Age=0;
        glm::vec3 RotationScale{1}, LocationScale{1};
    };
    std::vector<Shot> m_Shots;
    std::mt19937 m_Rng;
};
