#include "EditorUIPrimitives.h"
#include "CurveEditor.h"
#include "ImCurveAdapter.h"
#include "EditorUIHelpers.h"
#include "../../extern/imcurve/imcurve_editor.hpp"
#include <map>
#include <set>
#include <cmath>
#include <cfloat>

namespace CurveEditor {
bool Preview(const char* id,const Curve& curve,const ImVec2& size,const Options& options) {
    float samples[96];
    for(int i=0;i<96;++i) samples[i]=curve.Evaluate(options.TimeMin+(options.TimeMax-options.TimeMin)*i/95.0f);
    ImGui::PlotLines(id,samples,96,0,nullptr,FLT_MAX,FLT_MAX,ImVec2(size.x>0?size.x:ImGui::GetContentRegionAvail().x,size.y));
    const bool hovered=ImGui::IsItemHovered();
    if(hovered)EditorUI::SetTooltip("Read-only preview. Double-click to edit in a curve window.");
    return hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
}
namespace {
struct Session {
    ImCurveEditor<float> Editor;
    bool Pending=false,NumericPending=false,Expanded=false,Snap=false,Link=false,Playing=false,KeyList=true;
    int LastFrame=0;ImVec2 Min{},Max{};
    ImGuiID RootWindow=0;
    float TimeStep=.01f,ValueStep=.01f,Playhead=0,InsertTime=0,InsertValue=0;
    float TimeOffset=0,ValueOffset=0,TimeScale=1,ValueScale=1;
    std::string Error;
};
std::map<std::pair<ImGuiContext*,ImGuiID>,Session> sessions;
std::set<ImGuiContext*> contexts;
}
bool OwnsKeyboard() {
    const auto* context=ImGui::GetCurrentContext();
    if (!context) return false;
    auto mouse=ImGui::GetIO().MousePos;
    for (const auto& entry:sessions) {
        const auto& s=entry.second;
        const bool focused=context->NavWindow&&context->NavWindow->RootWindow->ID==s.RootWindow;
        if (entry.first.first==context && s.LastFrame>=ImGui::GetFrameCount()-1 && (focused ||
            (mouse.x>=s.Min.x && mouse.x<=s.Max.x && mouse.y>=s.Min.y && mouse.y<=s.Max.y))) return true;
    }
    return false;
}
bool DrawCanvas(const char* id,Curve& curve,const ImVec2& requestedSize,const Options& o) {
    ImGui::PushID(id);
    auto* context=ImGui::GetCurrentContext();
    if (contexts.insert(context).second) {
        ImGuiContextHook hook{};
        hook.Type=ImGuiContextHookType_Shutdown;
        hook.Callback=[](ImGuiContext* destroyed,ImGuiContextHook*) {
            for (auto it=sessions.begin();it!=sessions.end();)
                if (it->first.first==destroyed) it=sessions.erase(it); else ++it;
            contexts.erase(destroyed);
        };
        ImGui::AddContextHook(context,&hook);
    }
    const int frame=ImGui::GetFrameCount();
    for (auto it=sessions.begin(); it!=sessions.end();) {
        if (it->first.first==context && frame-it->second.LastFrame>600) it=sessions.erase(it);
        else ++it;
    }
    auto& s=sessions[{ImGui::GetCurrentContext(),ImGui::GetID("##session")}];
    s.LastFrame=frame;
    auto incoming=ToImCurve(curve);
    if (incoming!=s.Editor.GetCurve()) {
        s.Editor.SetCurve(incoming); s.Editor.Fit(); s.Pending=false; s.NumericPending=false;
    }
    s.Editor.Color=o.Color; s.Editor.TimeMin=o.TimeMin;
    s.Editor.TimeMax=std::max(o.TimeMax,o.TimeMin+0.0001f);
    s.Editor.Normalize=Normalize; s.Editor.Insert=Insert;
    s.Editor.KeyboardHistory=!o.ExternalHistory;
    s.Editor.Snap=s.Snap;s.Editor.TimeSnap=s.TimeStep;s.Editor.ValueSnap=s.ValueStep;s.Editor.LinkTangents=s.Link;
    bool committed=false;
    auto selected=s.Editor.SelectedKeys();
    const auto apply=[&](Curve c) {curve=std::move(c);s.Editor.UpdateCurve(ToImCurve(curve),true);committed=true;};
    const bool keyboard=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&!ImGui::GetIO().WantTextInput&&!ImGui::IsAnyItemActive();
    const bool ctrl=ImGui::GetIO().KeyCtrl;
    if (EditorUIPrimitives::SecondaryButton("Fit")) s.Editor.Fit();
    ImGui::SameLine();
    ImGui::BeginDisabled(selected.empty());
    if(EditorUIPrimitives::SecondaryButton("Fit Selected")) {
        const auto& k=curve.Keys[selected.front()];ImCurveRect<float> view{{k.Time,k.Value},{k.Time,k.Value}};
        for(int i:selected)view.Expand({curve.Keys[i].Time,curve.Keys[i].Value});
        const float px=std::max(.02f,view.GetWidth()*.15f),py=std::max(.0001f,view.GetHeight()*.2f);
        view.Min.X-=px;view.Max.X+=px;view.Min.Y-=py;view.Max.Y+=py;s.Editor.SetViewport(view);
    }
    ImGui::EndDisabled();ImGui::SameLine();
    if(EditorUIPrimitives::SecondaryButton("Select All")||(keyboard&&ctrl&&ImGui::IsKeyPressed(ImGuiKey_A))) {
        selected.clear();for(int i=0;i<(int)curve.Keys.size();++i)selected.push_back(i);s.Editor.SelectKeys(selected);
    }
    ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Deselect")){selected.clear();s.Editor.SelectKeys({});}
    ImGui::SameLine();
    // Toolbar history changes must reach the asset before preset handling.
    if (committed) curve=FromImCurve(s.Editor.GetCurve());
    if (EditorUIPrimitives::SecondaryButton("Presets")) ImGui::OpenPopup("##presets");
    if (ImGui::BeginPopup("##presets")) {
        float amplitude=0;
        for (const auto& k:curve.Keys) if (std::abs(k.Value)>std::abs(amplitude)) amplitude=k.Value;
        if (std::abs(amplitude)<0.000001f) amplitude=o.PresetAmplitude;
        const float duration=s.Editor.TimeMax-o.TimeMin;
        const auto preset=[&](const char* name,Curve c,bool normalized=false) {
            if (!ImGui::MenuItem(name)) return;
            if (normalized) for (auto& k:c.Keys) {
                k.Time=o.TimeMin+k.Time*duration; k.Value*=amplitude;
                k.InTangent*=amplitude/duration; k.OutTangent*=amplitude/duration;
            }
            curve=std::move(c); committed=true;
        };
        preset("Flat (0)",Curve::Line(o.TimeMin,0,s.Editor.TimeMax,0));
        preset("Linear",Curve::Line(o.TimeMin,0,s.Editor.TimeMax,amplitude));
        preset("Ease in/out",Curve::EaseInOut(),true);
        preset("Kick and settle",Curve::Kick(0.1f),true);
        Curve sine; constexpr float pi2=6.28318530718f;
        for (int i=0;i<=8;++i) {
            float t=float(i)/8,slope=pi2*std::cos(pi2*t);
            sine.Keys.push_back({t,i==0||i==8?0:std::sin(pi2*t),slope,slope});
        }
        preset("Sine",sine,true);
        if (ImGui::MenuItem("Smooth all tangents")) { curve.AutoTangents();for(auto& k:curve.Keys)k.Interpolation=CurveInterpolation::Cubic;committed=true; }
        if (o.Default) { ImGui::Separator(); preset("Reset to default",*o.Default); }
        ImGui::EndPopup();
    }
    if (committed) { s.Editor.UpdateCurve(ToImCurve(curve),true); s.Editor.SelectKeys({});selected.clear();s.Editor.Fit(); }
    const auto copy=[&] {
        Curve copied;for(int i:selected)copied.Keys.push_back(curve.Keys[i]);
        ImGui::SetClipboardText(nlohmann::json{{"tartarusCurve",copied.ToJson(false)}}.dump().c_str());
    };
    ImGui::BeginDisabled(selected.empty());
    if(EditorUIPrimitives::SecondaryButton("Copy")||(keyboard&&ctrl&&ImGui::IsKeyPressed(ImGuiKey_C)&&!selected.empty()))copy();
    ImGui::SameLine();const bool cut=EditorUIPrimitives::SecondaryButton("Cut")||(keyboard&&ctrl&&ImGui::IsKeyPressed(ImGuiKey_X)&&!selected.empty());
    ImGui::SameLine();const bool remove=EditorUIPrimitives::SecondaryButton("Delete Keys")||cut||(keyboard&&ImGui::IsKeyPressed(ImGuiKey_Delete)&&!selected.empty());
    if(remove&&!selected.empty()) {
        if(cut)copy();Curve c=curve;for(auto it=selected.rbegin();it!=selected.rend();++it)c.Keys.erase(c.Keys.begin()+*it);
        apply(std::move(c));selected.clear();s.Editor.SelectKeys({});
    }
    ImGui::EndDisabled();ImGui::SameLine();
    if(EditorUIPrimitives::SecondaryButton("Paste at Playhead")||(keyboard&&ctrl&&ImGui::IsKeyPressed(ImGuiKey_V))) {
        const char* text=ImGui::GetClipboardText();const auto j=nlohmann::json::parse(text?text:"",nullptr,false);Curve pasted;
        if(j.is_object()&&j.contains("tartarusCurve")&&Curve::FromJson(j["tartarusCurve"],pasted)&&!pasted.Empty()) {
            Curve c=curve;const float origin=pasted.StartTime();std::vector<float> times;
            for(auto k:pasted.Keys) {
                k.Time+=s.Playhead-origin;if(k.Time<o.TimeMin||k.Time>s.Editor.TimeMax)continue;
                const auto existing=std::find_if(c.Keys.begin(),c.Keys.end(),[&](const CurveKey& key){return std::abs(key.Time-k.Time)<.0001f;});
                if(existing==c.Keys.end())c.Keys.push_back(k);else *existing=k;times.push_back(k.Time);
            }
            c.Sort();apply(std::move(c));selected.clear();
            for(int i=0;i<(int)curve.Keys.size();++i)if(std::find(times.begin(),times.end(),curve.Keys[i].Time)!=times.end())selected.push_back(i);
            s.Editor.SelectKeys(selected);s.Error.clear();
        }else s.Error="Clipboard does not contain curve keys.";
    }
    ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Add Key")){s.InsertTime=s.Playhead;s.InsertValue=curve.Evaluate(s.Playhead);ImGui::OpenPopup("addKey");}
    if(ImGui::BeginPopup("addKey")) {
        ImGui::InputFloat("Time",&s.InsertTime,0,0,"%.5f");ImGui::InputFloat("Value",&s.InsertValue,0,0,o.ValueFormat);
        if(EditorUIPrimitives::SecondaryButton("Insert")&&std::isfinite(s.InsertTime)&&std::isfinite(s.InsertValue)) {
            Curve c=curve;const float time=std::clamp(s.InsertTime,o.TimeMin,s.Editor.TimeMax);
            const auto existing=std::find_if(c.Keys.begin(),c.Keys.end(),[&](const CurveKey& k){return std::abs(k.Time-time)<.0001f;});
            const int i=existing==c.Keys.end()?c.AddKey(time):int(existing-c.Keys.begin());c.Keys[i].Value=s.InsertValue;
            if(o.LinearFallback)c.Keys[i].Interpolation=CurveInterpolation::Linear;
            apply(std::move(c));selected={i};s.Editor.SelectKeys(selected);ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::TextUnformatted("Interpolation / tangents");ImGui::SameLine();ImGui::BeginDisabled(selected.empty());
    const char* modes[]={"Auto","Flat","Linear","Constant","Unify","Break"};
    for(int mode=0;mode<6;++mode) {
        if(mode)ImGui::SameLine();
        if(EditorUIPrimitives::SecondaryButton(modes[mode])) {
            Curve c=curve,smooth=curve;smooth.AutoTangents();
            for(int i:selected) {
                auto& k=c.Keys[i];k.Interpolation=mode==2?CurveInterpolation::Linear:mode==3?CurveInterpolation::Constant:CurveInterpolation::Cubic;
                if(mode==0){k.InTangent=smooth.Keys[i].InTangent;k.OutTangent=smooth.Keys[i].OutTangent;}
                if(mode==1)k.InTangent=k.OutTangent=0;
                if(mode==4)k.InTangent=k.OutTangent=(k.InTangent+k.OutTangent)*.5f;
            }
            if(mode==4)s.Link=true;if(mode==5)s.Link=false;apply(std::move(c));
        }
    }
    ImGui::EndDisabled();ImGui::SameLine();ImGui::Checkbox("Link Handles",&s.Link);
    ImGui::Checkbox("Snap",&s.Snap);ImGui::SameLine();ImGui::SetNextItemWidth(85);
    ImGui::DragFloat("Time Step",&s.TimeStep,.001f,.0001f,100,"%.4f",ImGuiSliderFlags_AlwaysClamp);ImGui::SameLine();ImGui::SetNextItemWidth(85);
    ImGui::DragFloat("Value Step",&s.ValueStep,.0001f,.000001f,100,"%.5f",ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();ImGui::BeginDisabled(selected.empty());if(EditorUIPrimitives::SecondaryButton("Transform"))ImGui::OpenPopup("transform");
    ImGui::SameLine();if(EditorUIPrimitives::SecondaryButton("Invert Values")){Curve c=curve;for(int i:selected){auto& k=c.Keys[i];k.Value=-k.Value;k.InTangent=-k.InTangent;k.OutTangent=-k.OutTangent;}apply(std::move(c));}
    ImGui::EndDisabled();
    if(ImGui::BeginPopup("transform")) {
        ImGui::InputFloat("Time Offset",&s.TimeOffset);ImGui::InputFloat("Value Offset",&s.ValueOffset);
        ImGui::InputFloat("Time Scale",&s.TimeScale);ImGui::InputFloat("Value Scale",&s.ValueScale);
        ImGui::TextDisabled("Time scales around the first selected key; values scale around zero.");
        if(EditorUIPrimitives::SecondaryButton("Apply")&&!selected.empty()) {
            if(std::isfinite(s.TimeOffset)&&std::isfinite(s.ValueOffset)&&std::isfinite(s.TimeScale)&&std::isfinite(s.ValueScale)&&s.TimeScale>0) {
                Curve c=curve;const float pivot=c.Keys[selected.front()].Time;bool valid=true;
                for(int i:selected){auto& k=c.Keys[i];k.Time=pivot+(k.Time-pivot)*s.TimeScale+s.TimeOffset;k.Value=k.Value*s.ValueScale+s.ValueOffset;k.InTangent*=s.ValueScale/s.TimeScale;k.OutTangent*=s.ValueScale/s.TimeScale;valid&=k.Time>=o.TimeMin&&k.Time<=s.Editor.TimeMax&&std::isfinite(k.Value)&&std::isfinite(k.InTangent)&&std::isfinite(k.OutTangent);}
                c.Sort();for(size_t i=1;i<c.Keys.size();++i)valid&=c.Keys[i].Time-c.Keys[i-1].Time>=.0001f;
                if(valid){apply(std::move(c));s.Editor.SelectKeys({});selected.clear();s.Error.clear();ImGui::CloseCurrentPopup();}
                else s.Error="Transform would overlap keys or leave the time range.";
            }else s.Error="Use finite values and a positive time scale.";
        }
        ImGui::EndPopup();
    }
    if(EditorUIPrimitives::SecondaryButton(s.Playing?"Pause":"Play"))s.Playing=!s.Playing;ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(140.0f,ImGui::GetContentRegionAvail().x-210));ImGui::SliderFloat("##playhead",&s.Playhead,o.TimeMin,s.Editor.TimeMax,"Time %.5f");
    if(s.Playing)s.Playhead=o.TimeMin+std::fmod(std::max(0.0f,s.Playhead-o.TimeMin)+ImGui::GetIO().DeltaTime,s.Editor.TimeMax-o.TimeMin);
    ImGui::SameLine();ImGui::Text("Value %.5g",curve.Evaluate(s.Playhead));ImGui::SameLine();ImGui::Checkbox("Key List",&s.KeyList);
    if(!s.Error.empty())ImGui::TextWrapped("%s",s.Error.c_str());
    ImVec2 size(requestedSize.x>0?requestedSize.x:ImGui::GetContentRegionAvail().x,
                std::max(140.0f,std::min(requestedSize.y,ImGui::GetContentRegionAvail().y-ImGui::GetFontSize()*(s.KeyList?11:4))));
    s.Editor.Draw("##graph",size,{s.Playhead});
    const auto graphItem=ImGui::GetCurrentContext()->LastItemData;
    s.Min=ImGui::GetWindowPos();s.Max=s.Min+ImGui::GetWindowSize();
    s.RootWindow=ImGui::GetCurrentWindow()->RootWindow->ID;
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Double-click: add key   Ctrl-click: select multiple\nDrag empty space: select   Right-drag: pan   Wheel: zoom\nRight-click key: tangents   Delete: remove   Ctrl+Z / Ctrl+Y: undo / redo");
    const auto edited=s.Editor.GetCurve();
    if (edited!=incoming) s.Pending=true;
    curve=FromImCurve(edited);
    if (s.Pending && !s.Editor.IsDragging()) { committed=true; s.Pending=false; }
    selected=s.Editor.SelectedKeys();
    if(s.KeyList&&ImGui::BeginTable("##keys",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders|ImGuiTableFlags_ScrollY,{0,ImGui::GetFontSize()*6})) {
        ImGui::TableSetupColumn("Key");ImGui::TableSetupColumn("Time");ImGui::TableSetupColumn("Value");ImGui::TableSetupColumn("Interpolation");ImGui::TableHeadersRow();
        for(int i=0;i<(int)curve.Keys.size();++i) {
            ImGui::PushID(i);ImGui::TableNextRow();ImGui::TableNextColumn();const bool has=std::find(selected.begin(),selected.end(),i)!=selected.end();
            if(ImGui::Selectable(std::to_string(i+1).c_str(),has)){if(!ctrl)selected={i};else if(has)selected.erase(std::find(selected.begin(),selected.end(),i));else selected.push_back(i);s.Editor.SelectKeys(selected);}
            const auto& k=curve.Keys[i];ImGui::TableNextColumn();ImGui::Text("%.5f",k.Time);ImGui::TableNextColumn();ImGui::Text(o.ValueFormat,k.Value);ImGui::TableNextColumn();
            ImGui::TextUnformatted(k.Interpolation==CurveInterpolation::Linear?"Linear":k.Interpolation==CurveInterpolation::Constant?"Constant":"Cubic");ImGui::PopID();
        }
        ImGui::EndTable();
    }
    int primary=s.Editor.Selected();
    if (primary>=0 && primary<int(curve.Keys.size())) {
        auto& key=curve.Keys[primary];
        float fields[4]={key.Time,key.Value,key.InTangent,key.OutTangent};
        bool numeric=false;bool deactivated=false;const char* labels[]={"Time","Value","In Slope","Out Slope"};
        for(int f=0;f<4;++f){if(f)ImGui::SameLine();ImGui::SetNextItemWidth(std::max(65.0f,(ImGui::GetContentRegionAvail().x-180)/(4-f)));
            numeric|=ImGui::DragFloat(labels[f],&fields[f],f==0?.001f:.0001f,0,0,f==0?"%.5f":o.ValueFormat);deactivated|=ImGui::IsItemDeactivatedAfterEdit();}
        if (numeric) {
            float lo=primary>0?curve.Keys[primary-1].Time+0.0001f:o.TimeMin;
            float hi=primary+1<int(curve.Keys.size())?curve.Keys[primary+1].Time-0.0001f:s.Editor.TimeMax;
            if (std::all_of(std::begin(fields),std::end(fields),[](float v){return std::isfinite(v);})) {
                const auto mode=key.Interpolation;
                const bool tangentChanged=fields[2]!=key.InTangent||fields[3]!=key.OutTangent;
                if(s.Link&&tangentChanged){const float slope=fields[2]!=key.InTangent?fields[2]:fields[3];fields[2]=fields[3]=slope;}
                key={std::clamp(fields[0],lo,std::max(lo,hi)),fields[1],fields[2],fields[3],tangentChanged?CurveInterpolation::Cubic:mode};
                s.Editor.UpdateCurve(ToImCurve(curve),!s.NumericPending);
                s.NumericPending=true;
            }
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Time | Value | Incoming slope | Outgoing slope");
        if (deactivated) { committed=true; s.NumericPending=false; }
    }
    ImGui::TextDisabled("%zu keys | %zu selected | X: %s  Y: value",curve.Keys.size(),selected.size(),o.TimeInSeconds?"seconds":"normalized time");
    ImGui::GetCurrentContext()->LastItemData=graphItem;
    ImGui::PopID(); return committed;
}
}
