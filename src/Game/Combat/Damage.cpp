#include "Damage.h"
#include "FirstPersonAnimation.h"
#include "Scripting/GameFrames.h"
#include "Scripting/ScriptRuntime.h"
#include <json.hpp>
#include <stdexcept>
namespace {
Scripting::DamageFrame Run(Scripting::DamageFrame f,int operation) {
    f.Operation=operation;
    if(!Scripting::InvokeProject("damage",&f,sizeof f))throw std::runtime_error("Project damage implementation unavailable");
    return f;
}
}
float DamageFalloff(float distance,float start,float end,float minScale) {
    Scripting::DamageFrame f;f.Distance=distance;f.Start=start;f.End=end;f.MinScale=minScale;return Run(f,0).Result;
}
float DamageForHit(const FirstPersonWeaponGameplay& w,HitZone zone,float distance) {
    Scripting::DamageFrame f;f.Distance=distance;f.Start=w.FalloffStart;f.End=w.FalloffEnd;f.MinScale=w.FalloffMin;
    f.Damage=w.Damage;f.HeadMultiplier=w.HeadMultiplier;f.LimbMultiplier=w.LimbMultiplier;f.Zone=(int)zone;return Run(f,1).Result;
}
HitZone ZoneFromCapsuleHeight(float hitY,float footY,float height) {
    Scripting::DamageFrame f;f.HitY=hitY;f.FootY=footY;f.Height=height;return (HitZone)Run(f,2).Zone;
}
HitZone ZoneOfRegion(HitRegion region) {Scripting::DamageFrame f;f.Region=(int)region;return (HitZone)Run(f,3).Zone;}
HitRegion RegionFromBone(const char* bone) {
    std::string result;
    if(!Scripting::RequestProject("bone-region",nlohmann::json(bone?bone:"").dump(),result))throw std::runtime_error("Project bone mapping unavailable");
    return (HitRegion)nlohmann::json::parse(result).get<int>();
}
HitRegion RegionFromPart(int part) {Scripting::DamageFrame f;f.Part=part;return (HitRegion)Run(f,4).Region;}
float DamageIndicatorAngle(const glm::vec3& camera,float yaw,const glm::vec3& source) {
    Scripting::DamageFrame f;f.Camera={camera.x,camera.y,camera.z};f.Source={source.x,source.y,source.z};f.Yaw=yaw;return Run(f,5).Result;
}
