#include "RecoilAssetEditor.h"
#include "EditorPropertyRows.h"
#include "CurveEditor.h"
#include "FirstPersonProcedural.h"
#include <algorithm>
#include <cfloat>
#include <vector>
#include <glm/gtx/euler_angles.hpp>
namespace {
bool Tree(const char* label,bool open=false) {
    return ImGui::TreeNodeEx(label,ImGuiTreeNodeFlags_SpanAvailWidth|(open?ImGuiTreeNodeFlags_DefaultOpen:0));
}
}
void RecoilAssetEditor::Draw(PropertyRows& r, WeaponRecoilSettings& settings, float rpm, int forcedTab) {
    auto& d=settings.Unity;
    auto* storage=ImGui::GetStateStorage();
    const ImGuiID heightId=ImGui::GetID("##recoilPreviewHeight");
    float previewHeight=storage->GetFloat(heightId,260.0f);
    const float available=ImGui::GetContentRegionAvail().y;
    previewHeight=std::clamp(previewHeight,120.0f,std::max(120.0f,available-100.0f));
    ImGui::BeginChild("##recoilSettings",{0,std::max(100.0f,available-previewHeight-10.0f)},ImGuiChildFlags_None);
    const auto number=[&](const char* label,float& value,float lo=-FLT_MAX,float hi=FLT_MAX) {
        r.Float(label,value,0.01f,lo,hi,"%.3f");
    };
    const auto four=[&](const char* label,glm::vec4& value) {
        ImGui::PushID(label); r.Label(label,"Two random ranges (X..Y and Z..W); choose a side with equal probability.");
        ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragFloat4("##value",&value.x,0.01f,0,0,"%.3f");
        if (ImGui::IsItemDeactivatedAfterEdit()) r.MarkChanged(); ImGui::PopID();
    };
    const auto progress=[&](const char* id,RecoilProgression& p) {
        ImGui::PushID(id); r.Heading(id); number("Acceleration",p.Acceleration);
        number("Damping",p.Damping); number("Amount",p.Amount); ImGui::PopID();
    };
    const auto curves=[&](const char* id,RecoilVectorCurve& c) {
        if (!Tree(id,true)) return;
        ImGui::PushID(id);
        float end=ImGui::GetStateStorage()->GetFloat(ImGui::GetID("end"),std::max(c.Length(),1.0f));
        ImGui::SetNextItemWidth(ImGui::GetFontSize()*6);
        ImGui::DragFloat("View end (s)",&end,0.01f,0.01f,60,"%.3f");
        end=std::clamp(end,0.01f,60.0f); ImGui::GetStateStorage()->SetFloat(ImGui::GetID("end"),end);
        CurveEditor::Options o; o.TimeMax=end; o.ValueFormat="%.3f";o.TimeInSeconds=true;
        const char* names[]={"X","Y","Z"}; Curve* axes[]={&c.X,&c.Y,&c.Z};
        for (int i=0;i<3;++i) {
            ImGui::TextUnformatted(names[i]);
            o.Title=names[i];
            if(CurveEditor::Draw(names[i],*axes[i],{0,ImGui::GetFontSize()*6},o))r.MarkChanged();
        }
        ImGui::PopID(); ImGui::TreePop();
    };
    r.Note("RecoilAnimData: Unity local axes (+Z forward, negative pitch raises the muzzle). Curves are interpolation alphas keyed in seconds.");
    if (ImGui::BeginTabBar("##recoildata")) {
        if (ImGui::BeginTabItem("Recoil Targets",nullptr,forcedTab==0?ImGuiTabItemFlags_SetSelected:0)) {
            r.Heading("Rotation Targets"); r.Vec2("Pitch",d.Pitch,0.01f,"%.3f deg"); four("Roll",d.Roll); four("Yaw",d.Yaw);
            r.Heading("Translation Targets"); r.Vec2("Kickback",d.Kickback,0.001f,"%.4f m");
            r.Vec2("Kick Up",d.KickUp,0.001f,"%.4f m"); r.Vec2("Kick Right",d.KickRight,0.001f,"%.4f m");
            r.Heading("Aiming Multipliers"); r.Vec3("Aim Rot",d.AimRot,0.01f,"%.3f"); r.Vec3("Aim Loc",d.AimLoc,0.01f,"%.3f");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Smoothing",nullptr,forcedTab==1?ImGuiTabItemFlags_SetSelected:0)) {
            r.Heading("Auto / Burst Settings"); r.Vec3("Smooth Rot",d.SmoothRot,0.1f,"%.3f"); r.Vec3("Smooth Loc",d.SmoothLoc,0.1f,"%.3f");
            r.Vec3("Extra Rot",d.ExtraRot,0.01f,"%.3f","Zero means one, matching Unity.");
            r.Vec3("Extra Loc",d.ExtraLoc,0.01f,"%.3f","Zero means one, matching Unity."); ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Layers",nullptr,forcedTab==2?ImGuiTabItemFlags_SetSelected:0)) {
            r.Heading("Noise Layer"); r.Vec2("Noise X",d.NoiseX,0.001f,"%.4f"); r.Vec2("Noise Y",d.NoiseY,0.001f,"%.4f");
            r.Vec2("Noise Accel",d.NoiseAccel,0.1f,"%.3f"); r.Vec2("Noise Damp",d.NoiseDamp,0.1f,"%.3f"); number("Noise Scalar",d.NoiseScalar,0,1);
            r.Heading("Pushback Layer"); number("Push Amount",d.PushAmount); number("Push Accel",d.PushAccel); number("Push Damp",d.PushDamp);
            r.Heading("Recoil Sway"); r.Vec2("Pitch Sway",d.Sway.PitchSway,0.01f,"%.3f"); r.Vec2("Yaw Sway",d.Sway.YawSway,0.01f,"%.3f");
            number("Roll Multiplier",d.Sway.RollMultiplier); number("Damping",d.Sway.Damping); number("Acceleration",d.Sway.Acceleration);
            number("ADS Scale",d.Sway.AdsScale,0,1); r.Vec3("Pivot Offset",d.Sway.PivotOffset,0.001f,"%.4f m");
            progress("Pitch Progress",d.PitchProgress); progress("Up Progress",d.UpProgress); number("ADS Progress Alpha",d.AdsProgressAlpha,0,1);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Misc",nullptr,forcedTab==3?ImGuiTabItemFlags_SetSelected:0)) {
            r.Heading("Controller Recoil"); r.Vec2("Horizontal Recoil",d.HorizontalRecoil,0.01f,"%.3f deg"); r.Vec2("Vertical Recoil",d.VerticalRecoil,0.01f,"%.3f deg");
            number("Horizontal Smoothing",d.HorizontalSmoothing,0); number("Vertical Smoothing",d.VerticalSmoothing,0); number("Damping",d.Damping,0);
            r.Heading("Recoil Motion"); r.Vec3("Hip Pivot Offset",d.HipPivotOffset,0.001f,"%.4f m"); r.Vec3("Aim Pivot Offset",d.AimPivotOffset,0.001f,"%.4f m");
            r.Check("Smooth Roll",d.SmoothRoll,"Alternates roll when consecutive targets have the same sign."); number("Play Rate",d.PlayRate,0);
            r.Heading("Curves"); curves("Semi Rot Curve",d.SemiRotCurve); curves("Semi Loc Curve",d.SemiLocCurve);
            curves("Auto Rot Curve",d.AutoRotCurve); curves("Auto Loc Curve",d.AutoLocCurve);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::InvisibleButton("##recoilPreviewSplitter",{ImGui::GetContentRegionAvail().x,8});
    if(ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if(ImGui::IsItemActive()) previewHeight-=ImGui::GetIO().MouseDelta.y;
    storage->SetFloat(heightId,std::clamp(previewHeight,120.0f,std::max(120.0f,available-100.0f)));
    ImGui::GetWindowDrawList()->AddLine(ImGui::GetItemRectMin(),ImGui::GetItemRectMin()+ImVec2(ImGui::GetItemRectSize().x,0),ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::BeginChild("##recoilPreview",{0,previewHeight},ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Recoil Preview");
    static int mode=0; static bool aiming=false;
    ImGui::Combo("Preview",&mode,"Semi\0Burst (3)\0Auto (10)\0"); ImGui::Checkbox("Preview Aiming",&aiming);
    static int seed=42; ImGui::InputInt("Pattern seed",&seed);
    RecoilAnimation state; std::mt19937 rng(static_cast<unsigned>(seed));
    const float dt=1.0f/120,interval=60/std::max(rpm,1.0f);
    const int count=mode==0?1:mode==1?3:10;
    const float recovery=std::max({d.SemiRotCurve.Length(),d.SemiLocCurve.Length(),d.AutoRotCurve.Length(),d.AutoLocCurve.Length()})/std::max(d.PlayRate,0.01f);
    const float duration=std::clamp(count*interval+recovery+2.0f,2.0f,30.0f);
    const int frames=static_cast<int>(duration/dt)+1;
    std::vector<float> channels[8]; for(auto& channel:channels) channel.resize(frames);
    int shot=0; bool stopped=false; glm::vec2 total(0);
    for(int f=0;f<frames;++f) {
        const double time=f*static_cast<double>(dt);
        if(shot<count && time+1e-6>=shot*interval) {
            state.Play(d,aiming,rpm,static_cast<RecoilFireMode>(mode),rng,time,dt); ++shot;
        }
        if(!stopped && shot==count && time>=count*interval) { state.Stop(); stopped=true; }
        state.Update(d,aiming,dt,rng);
        const glm::quat q(state.OutRot.w,-state.OutRot.x,-state.OutRot.y,state.OutRot.z);
        float yaw,pitch,roll; glm::extractEulerAngleYXZ(glm::mat4_cast(q),yaw,pitch,roll);
        const glm::vec3 rot=glm::degrees(glm::vec3(pitch,yaw,roll));
        const glm::vec3 loc=state.OutLoc*glm::vec3(1,1,-1);
        total+=state.RecoilDelta;
        for(int a=0;a<3;++a) {channels[a][f]=rot[a]; channels[a+3][f]=loc[a];}
        channels[6][f]=total.y; channels[7][f]=total.x;
    }
    ImGui::TextDisabled("%.2f s | %d shots | %.0f RPM | 120 Hz | no look compensation",duration,count,rpm);
    const char* labels[]={"Pitch (deg)","Yaw (deg)","Roll (deg)","Right (m)","Up (m)","Back (m)","Look pitch (deg)","Look yaw (deg)"};
    static int channel=0;
    ImGui::Combo("Channel",&channel,"Pitch (deg)\0Yaw (deg)\0Roll (deg)\0Right (m)\0Up (m)\0Back (m)\0Look pitch (deg)\0Look yaw (deg)\0");
    float lo=0,hi=0; for(float v:channels[channel]) {lo=std::min(lo,v);hi=std::max(hi,v);}
    const float pad=std::max((hi-lo)*0.1f,channel>=3 && channel<=5?0.001f:0.1f);
    ImGui::TextDisabled("%s | %.4f .. %.4f",labels[channel],lo,hi);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PlotLines("##recoilOutput",channels[channel].data(),frames,0,nullptr,lo-pad,hi+pad,{ImGui::GetContentRegionAvail().x,std::max(60.0f,ImGui::GetContentRegionAvail().y-25)});
    if(ImGui::IsItemHovered()) {
        const float fraction=std::clamp((ImGui::GetIO().MousePos.x-ImGui::GetItemRectMin().x)/std::max(ImGui::GetItemRectSize().x,1.0f),0.0f,1.0f);
        const int sample=std::min(static_cast<int>(fraction*(frames-1)),frames-1);
        ImGui::SetTooltip("%.3f s: %.5f\nRange %.5f .. %.5f",sample*dt,channels[channel][sample],lo,hi);
    }
    ImGui::TextDisabled("0 s                                              %.2f s",duration);
    ImGui::EndChild();
}
