#include "UnitTestSupport.h"
#include "FirstPersonProcedural.h"
#include <glm/gtx/euler_angles.hpp>
#include <cmath>

namespace {
bool Near(float a,float b,float epsilon=1e-5f) { return std::abs(a-b)<epsilon; }
RecoilAnimData Fixture() {
    RecoilAnimData d; d.PlayRate=1; d.Pitch={-10,-10}; d.Yaw={2,2,2,2}; d.Roll={3,3,3,3};
    d.KickRight={0.02f,0.02f}; d.KickUp={0.03f,0.03f}; d.Kickback={-0.1f,-0.1f};
    Curve triangle; triangle.Keys={{0,0,0,10},{0.1f,1,10,-5},{0.3f,0,-5,0}};
    d.SemiRotCurve=d.SemiLocCurve=d.AutoRotCurve=d.AutoLocCurve={triangle,triangle,triangle};
    d.AimRot={0.5f,0.25f,0.75f}; d.AimLoc={0.25f,0.5f,0.75f};
    return d;
}
void TestRecoilAnimDataPort() {
    RecoilAsset asset,parsed;
    auto j=nlohmann::json::parse(asset.ToJsonString());
    CHECK(j["version"]==2 && j["recoil"].size()==34);
    CHECK(j["recoil"]["yaw"].size()==4 && j["recoil"]["roll"].size()==4);
    CHECK(!j["recoil"].contains("duration") && !j["recoil"].contains("cameraPitch"));
    CHECK(asset.Data.Unity.PlayRate==0 && asset.Data.Unity.AimRot==glm::vec3(0));
    CHECK(asset.Data.Unity.NoiseScalar==1 && asset.Data.Unity.AdsProgressAlpha==1);
    asset.Data.Unity=Fixture(); asset.Data.Unity.Sway={glm::vec2(-1,2),glm::vec2(-3,4),0.5f,4,8,0.3f,{0,0,0.2f}};
    asset.Data.Unity.PitchProgress={3,4,5}; asset.Data.Unity.UpProgress={6,7,8};
    CHECK(RecoilAsset::FromJsonString(asset.ToJsonString(),parsed));
    CHECK(parsed.ToJsonString()==asset.ToJsonString());
    asset.Data.Unity.SemiRotCurve.X.Keys[0]={0.000000123456f,0.000000234567f,-17.123456f,0.000000345678f};
    CHECK(RecoilAsset::FromJsonString(asset.ToJsonString(),parsed));
    CHECK(parsed.Data.Unity.SemiRotCurve.X.Keys[0].Time==asset.Data.Unity.SemiRotCurve.X.Keys[0].Time);
    CHECK(parsed.Data.Unity.SemiRotCurve.X.Keys[0].OutTangent==asset.Data.Unity.SemiRotCurve.X.Keys[0].OutTangent);
    const auto before=parsed.ToJsonString();
    for (int bad=0;bad<5;++bad) {
        j=nlohmann::json::parse(before);
        if (bad==0) j["recoil"]["yaw"]={1,2};
        if (bad==1) j["recoil"]["playRate"]=-1;
        if (bad==2) j["recoil"]["noiseScalar"]=2;
        if (bad==3) j["recoil"]["recoilCurves"]["autoRotCurve"]["x"]=nlohmann::json::array();
        if (bad==4) j["recoil"]["horizontalSmoothing"]="fast";
        CHECK(!RecoilAsset::FromJsonString(j.dump(),parsed)); CHECK(parsed.ToJsonString()==before);
    }
    auto legacy=WeaponProceduralSettings::Defaults();
    j={{"type","recoil"},{"version",1},{"recoil",legacy.ToJson()["recoil"]}};
    CHECK(RecoilAsset::FromJsonString(j.dump(),parsed)); CHECK(parsed.Data.UnityPort);
    CHECK(Near(parsed.Data.Unity.SemiRotCurve.Length(),legacy.Recoil.Duration));
    CHECK(parsed.Data.Unity.Pitch.x<0 && parsed.Data.Unity.Kickback.x<=0);
    WeaponProceduralSettings embedded=legacy; embedded.Recoil=asset.Data;
    auto restored=legacy; CHECK(WeaponProceduralSettings::FromJson(embedded.ToJson(),restored,nullptr));
    CHECK(restored.Recoil.UnityPort && restored.Recoil.Unity.ToJson()==asset.Data.Unity.ToJson());
}
void TestRecoilAnimationPort() {
    auto d=Fixture(); std::mt19937 rng(7);
    RecoilAnimation state;
    state.Play(d,false,600,RecoilFireMode::Semi,rng);
    state.Update(d,false,0.05f,rng); CHECK(state.OutLoc==glm::vec3(0)); // solver before timeline
    state.Update(d,false,0.05f,rng);
    CHECK(glm::length(state.OutLoc-glm::vec3(0.01f,0.015f,-0.05f))<1e-5f);
    glm::quat expected=glm::quat_cast(glm::yawPitchRoll(glm::radians(1.0f),glm::radians(-5.0f),glm::radians(1.5f)));
    CHECK(std::abs(glm::dot(state.OutRot,expected))>0.99999f);
    // Aim scales are chosen at target generation, not continuously blended onto a shot.
    state=RecoilAnimation{}; state.Play(d,true,600,RecoilFireMode::Semi,rng);
    state.Update(d,true,0.05f,rng); state.Update(d,true,0.05f,rng);
    CHECK(glm::length(state.OutLoc-glm::vec3(0.0025f,0.0075f,-0.0375f))<1e-5f);
    auto aimed=state.OutLoc; state.Update(d,false,0,rng); CHECK(glm::length(state.OutLoc-aimed*2.0f)<1e-5f); // next alpha at 0.1, target stays aimed
    // First auto round is semi; rapid subsequent Play enters the looping state.
    state=RecoilAnimation{}; state.Play(d,false,600,RecoilFireMode::Auto,rng); CHECK(!state.IsLooping());
    state.Update(d,false,0.05f,rng); state.Play(d,false,600,RecoilFireMode::Auto,rng); CHECK(state.IsLooping());
    d.ExtraRot=glm::vec3(0); d.SmoothRot=glm::vec3(0); d.ExtraLoc=glm::vec3(0); d.SmoothLoc=glm::vec3(0);
    state.Update(d,false,0.05f,rng); state.Update(d,false,0.05f,rng);
    CHECK(std::isfinite(state.OutLoc.z));
    state.Stop(); CHECK(!state.IsLooping() && state.IsPlaying());
    for (int i=0;i<15;++i) state.Update(d,false,0.05f,rng);
    CHECK(!state.IsPlaying());
    state=RecoilAnimation{};
    state.Play(d,false,600,RecoilFireMode::Auto,rng,10,0.025f);
    state.Update(d,false,0.025f,rng);
    state.Play(d,false,600,RecoilFireMode::Auto,rng,10.2,0.025f);
    CHECK(!state.IsLooping()); // state transitions use unscaled shot time, not scaled playback time
    state.Play(d,false,600,RecoilFireMode::Auto,rng,10.25,0.025f); CHECK(state.IsLooping());
    state.Stop(); state.Play(d,false,600,RecoilFireMode::Auto,rng,20,0.025f); CHECK(!state.IsLooping());
    d=Fixture(); state=RecoilAnimation{};
    state.Play(d,false,600,RecoilFireMode::Semi,rng); state.Update(d,false,0.05f,rng);
    d.SemiLocCurve.X=Curve::Line(0,1,1,1); state.Update(d,false,0.05f,rng);
    CHECK(Near(state.OutLoc.x,0.02f)); // active curve edits behave like Unity AnimationCurve references
    // Roll alternation follows the preceding target's sign, irrespective of random side.
    d=Fixture(); d.Pitch=glm::vec2(0); d.Yaw=glm::vec4(0); d.Roll=glm::vec4(4); d.SmoothRoll=true;
    state=RecoilAnimation{}; state.Play(d,false,600,RecoilFireMode::Semi,rng);
    state.Update(d,false,0.05f,rng); state.Update(d,false,0.05f,rng); CHECK(state.OutRot.z>0);
    state.Play(d,false,600,RecoilFireMode::Semi,rng);
    state.Update(d,false,0.05f,rng); state.Update(d,false,0.05f,rng);
    state.Update(d,false,0.05f,rng); CHECK(state.OutRot.z<0);
    // Per-axis layers: noise decays before following; progression follows before damping.
    d=RecoilAnimData{}; d.PlayRate=1; d.NoiseX=glm::vec2(1); d.NoiseY=glm::vec2(2);
    d.NoiseAccel={2,3}; d.NoiseDamp={4,5}; d.PitchProgress={2,3,4}; d.UpProgress={5,6,7};
    state=RecoilAnimation{}; state.Play(d,false,600,RecoilFireMode::Semi,rng); state.Update(d,false,0.1f,rng);
    CHECK(Near(state.OutLoc.x,std::exp(-0.4f)*(1-std::exp(-0.2f))));
    CHECK(Near(state.OutLoc.y,2*std::exp(-0.5f)*(1-std::exp(-0.3f))+7*(1-std::exp(-0.5f))));
    expected=glm::angleAxis(glm::radians(4*(1-std::exp(-0.2f))),glm::vec3(1,0,0));
    CHECK(std::abs(glm::dot(state.OutRot,expected))>0.99999f);
    // Composition: main pivot translation is applied before the independent sway rotation/pivot.
    d=Fixture(); d.HipPivotOffset={0,0,0.2f}; d.Sway.PitchSway=glm::vec2(2); d.Sway.YawSway=glm::vec2(3);
    d.Sway.Acceleration=10; d.Sway.RollMultiplier=0.5f; d.Sway.PivotOffset={0,0,0.1f};
    state=RecoilAnimation{}; state.Play(d,false,600,RecoilFireMode::Semi,rng); state.Update(d,false,0.1f,rng);
    float a=1-std::exp(-1.0f); auto sway=glm::quat_cast(glm::yawPitchRoll(glm::radians(3*a),glm::radians(2*a),glm::radians(1.5f*a)));
    CHECK(glm::length(state.OutLoc-(sway*d.Sway.PivotOffset-d.Sway.PivotOffset))<1e-5f);
    CHECK(std::abs(glm::dot(state.OutRot,sway))>0.99999f);
}
void TestUnityControllerRecoil() {
    RecoilAnimData d; d.VerticalRecoil=glm::vec2(2); d.HorizontalRecoil=glm::vec2(1);
    std::mt19937 rng(9); RecoilAnimation state;
    state.Play(d,false,600,RecoilFireMode::Semi,rng); state.Update(d,false,0.1f,rng);
    CHECK(state.RecoilDelta==glm::vec2(0)); // unlike the legacy solver, zero smoothing means alpha zero
    d.HorizontalSmoothing=d.VerticalSmoothing=std::log(2.0f)/0.1f; d.Damping=std::log(2.0f)/0.1f;
    state=RecoilAnimation{}; state.Play(d,false,600,RecoilFireMode::Semi,rng); state.Update(d,false,0.1f,rng);
    CHECK(glm::length(state.RecoilDelta-glm::vec2(0.5f,1))<1e-5f);
    state.UpdateDeltaInput({-0.25f,-0.5f}); state.Update(d,false,0,rng); state.UpdateDeltaInput({0,0}); state.Stop();
    state.Update(d,false,0.1f,rng); CHECK(state.RecoilDelta==glm::vec2(0)); // damp target after following
    state.Update(d,false,0.1f,rng); CHECK(glm::length(state.RecoilDelta-glm::vec2(-0.0625f,-0.125f))<1e-5f);
    // Controller target is unscaled in ADS, independent of the visual aim multipliers.
    RecoilAnimation hip,aim; hip.Play(d,false,600,RecoilFireMode::Semi,rng); aim.Play(d,true,600,RecoilFireMode::Semi,rng);
    hip.Update(d,false,0.1f,rng); aim.Update(d,true,0.1f,rng); CHECK(hip.RecoilDelta==aim.RecoilDelta);
    WeaponProceduralSettings s=WeaponProceduralSettings::Defaults(); s.Recoil.UnityPort=true; s.Recoil.Unity=d;
    s.Sway.Enabled=s.Bob.Enabled=s.Breath.Enabled=s.Lean.Enabled=false;
    WeaponProceduralState host; WeaponProceduralInput input; input.Dt=0.1f;
    host.OnShot(s,false); host.Update(s,input); CHECK(Near(host.ConsumeAimKick().x,1));
    CHECK(host.ConsumeAimKick()==glm::vec2(0));
    // Holding a semi trigger retains the kick for one shot interval, then recovers.
    // Automatic and burst fire keep their release-controlled recovery.
    for (auto mode : {RecoilFireMode::Semi,RecoilFireMode::Auto,RecoilFireMode::Burst}) {
        host.Reset(); input.Dt=0.05f;
        host.OnShot(s,false,false,300,mode); host.SetFiring(true);
        float angle=0;
        for (int i=0;i<3;++i) {
            host.Update(s,input); auto kick=host.ConsumeAimKick();
            CHECK(kick.x>0); angle+=kick.x;
        }
        float peak=angle; bool recovered=false;
        for (int i=0;i<30;++i) {
            host.Update(s,input); auto kick=host.ConsumeAimKick();
            recovered |= kick.x < 0; angle+=kick.x; peak=std::max(peak,angle);
        }
        CHECK(recovered == (mode==RecoilFireMode::Semi));
        if (mode==RecoilFireMode::Semi) CHECK(angle<peak*0.05f);
        else CHECK(std::abs(angle-2.0f)<0.001f);
    }
}
}
void RegisterRecoilPortTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("RecoilAnimDataPort",TestRecoilAnimDataPort);
    tests.emplace_back("RecoilAnimationPort",TestRecoilAnimationPort);
    tests.emplace_back("UnityControllerRecoil",TestUnityControllerRecoil);
}
