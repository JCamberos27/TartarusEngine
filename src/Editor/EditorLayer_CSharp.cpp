#include "EditorLayer.h"
#include "ScriptIDE.h"
#include "ProjectPaths.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "ComponentRegistry.h"
#include "World.h"
#include "Scripting/ScriptRuntime.h"
#include <imgui.h>
#include <json.hpp>
using namespace EditorInternal;
void EditorLayer::OpenScriptIDE(const std::string& path,int line,int column) {
    if(!m_ScriptIDE)m_ScriptIDE=std::make_unique<ScriptIDE>();
    if(path.empty())m_ScriptIDE->Show();else m_ScriptIDE->Open(path,line,column);
}
bool EditorLayer::HasUnsavedScripts() const {return m_ScriptIDE && m_ScriptIDE->HasUnsavedFiles();}
bool EditorLayer::ScriptIDEHasKeyboardFocus() const {return m_ScriptIDE && m_ScriptIDE->OwnsKeyboard();}
bool EditorLayer::SaveScripts() {return !m_ScriptIDE || m_ScriptIDE->SaveAll();}
void EditorLayer::DiscardScripts() {if(m_ScriptIDE)m_ScriptIDE->DiscardAll();}
namespace {
bool ScriptTextField(const char* label,std::string& value) {
    auto resize=[](ImGuiInputTextCallbackData* data)->int {
        if(data->EventFlag==ImGuiInputTextFlags_CallbackResize) {
            auto* text=static_cast<std::string*>(data->UserData);text->resize(static_cast<size_t>(data->BufTextLen));data->Buf=text->data();
        }
        return 0;
    };
    return ImGui::InputText(label,value.data(),value.capacity()+1,ImGuiInputTextFlags_CallbackResize,resize,&value);
}
}
int EditorLayer::ManagedEditorService(World& world,AssetLibrary& assets,int op,Scripting::NativeRequest& r,
                                    int& windows,int& widgets,int& groups,bool inlineInspector) {
    const char* text=r.Text?r.Text:"";
    if(op==100) {bool open=r.Result!=0;const bool visible=ImGui::Begin(text,&open);++windows;widgets=0;r.Result=open?1:0;return visible?1:0;}
    if(op==101) {if(!windows)return 0;while(groups>0){ImGui::TreePop();--groups;}ImGui::End();--windows;return 1;}
    if(op==110) {PushUndo(world,text);return 1;}
    if(op==111) {if(!world.Registry.valid(m_Selected))return 0;r.Entity=entt::to_integral(m_Selected);return 1;}
    if(op==112) {
        const auto e=static_cast<entt::entity>(r.Entity);
        if(e==entt::null)ClearSelection();else if(world.Registry.valid(e))SelectItem(e,false);else return 0;return 1;
    }
    if(op==113) {m_Dirty=true;return 1;}
    if(op==114) {Scripting::RequestBuild();return 1;}
    if(op==116) {static thread_local std::string result;result=text;r.Text=result.c_str();return 1;}
    if(op==126) {AtomicFile::NotifyWillChange(std::filesystem::u8path(text));return 1;}
    if(op==127) {m_RequestGlobalUndo=true;return 1;}
    if(op==128) {m_RequestGlobalRedo=true;return 1;}
    if(op==129) {if(*text)OpenScriptIDE(ProjectPaths::Resolve(text),r.Result,static_cast<int>(r.Value));else OpenScriptIDE();return 1;}
    if(!windows && !inlineInspector)return 0;
    if(op==117) {
        const bool open=ImGui::TreeNodeEx(text,ImGuiTreeNodeFlags_SpanAvailWidth|(r.Result?ImGuiTreeNodeFlags_DefaultOpen:0));
        if(open)++groups;return open?1:0;
    }
    if(op==118) {if(!groups)return 0;ImGui::TreePop();--groups;return 1;}
    if(op==122) {if(ImGui::IsItemHovered())EditorUI::SetTooltip("%s",text);return 1;}
    ImGui::PushID(++widgets);
    struct Pop {~Pop(){ImGui::PopID();}} pop;
    switch(op) {
    case 102:ImGui::TextUnformatted(text);return 1;
    case 103:return ActionButton(text,"Run this C# editor command")?1:0;
    case 104:{bool value=r.Result!=0;const bool changed=ImGui::Checkbox(text,&value);r.Result=value?1:0;return changed?1:0;}
    case 105:return ImGui::DragFloat(text,&r.Value,.05f)?1:0;
    case 106:return ImGui::DragInt(text,&r.Result,1)?1:0;
    case 107:return ImGui::DragFloat3(text,&r.A.x,.05f)?1:0;
    case 108:return ImGui::SliderFloat(text,&r.Value,r.A.x,r.A.y)?1:0;
    case 109:ImGui::Separator();return 1;
    case 115:{
        const auto data=nlohmann::json::parse(text);static thread_local std::string value;
        value=data.at("value").get<std::string>();const auto label=data.at("label").get<std::string>();
        const bool changed=ScriptTextField(label.c_str(),value);r.Text=value.c_str();return changed?1:0;
    }
    case 119:{
        const auto labels=nlohmann::json::parse(text).get<std::vector<std::string>>();
          const float right=ImGui::GetCursorScreenPos().x+ImGui::GetContentRegionAvail().x;
        for(size_t i=0;i<labels.size();++i) {
              const float width=ImGui::CalcTextSize(labels[i].c_str()).x+ImGui::GetStyle().FramePadding.x*2;
              if(i && ImGui::GetItemRectMax().x+ImGui::GetStyle().ItemSpacing.x+width<=right)ImGui::SameLine();
              ImGui::PushID(static_cast<int>(i));
              const bool active=r.Result==static_cast<int>(i);
              if(active)ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
              if(ImGui::Button(labels[i].c_str()))r.Result=static_cast<int>(i);
              if(active)ImGui::PopStyleColor();
            ImGui::PopID();
        }
        return 1;
    }
    case 120:ImGui::TextWrapped("%s",text);return 1;
    case 121:{
        const auto data=nlohmann::json::parse(text);const auto labels=data.at("labels").get<std::vector<std::string>>();
        const auto label=data.at("label").get<std::string>();
        const char* current=r.Result>=0 && static_cast<size_t>(r.Result)<labels.size()?labels[r.Result].c_str():"(unknown)";
        if(ImGui::BeginCombo(label.c_str(),current)) {
            for(size_t i=0;i<labels.size();++i)if(ImGui::Selectable(labels[i].c_str(),r.Result==static_cast<int>(i)))r.Result=static_cast<int>(i);
            ImGui::EndCombo();
        }
        return 1;
    }
    case 125:{
        const auto data=nlohmann::json::parse(text);const auto component=data.at("component").get<std::string>();const auto field=data.at("field").get<std::string>();
        const auto entity=static_cast<entt::entity>(r.Entity);if(!world.Registry.valid(entity))return 0;
        for(const auto& rc:ComponentRegistry::All())if(component==rc.Meta.Name && rc.Has(world.Registry,entity))
            for(const auto& f:rc.Meta.Fields)if(field==ReflectFieldKey(f)) {DrawReflectedField(world,assets,rc,f,{entity});return 1;}
        return 0;
    }
    }
    return 0;
}
void EditorLayer::DrawManagedEditorTools(World& world,AssetLibrary& assets,float dt) {
    int windows=0,widgets=0,groups=0;
    Scripting::DrawEditorScripts(world,assets,dt,[&](int op,Scripting::NativeRequest& r) {
        return ManagedEditorService(world,assets,op,r,windows,widgets,groups,false);
    });
    while(groups>0){ImGui::TreePop();--groups;}
    while(windows>0){ImGui::End();--windows;}
}
bool EditorLayer::DrawManagedInspector(World& world,AssetLibrary& assets,entt::entity entity,const std::string& type,
                                     std::string* fields,const std::string& metadata) {
    auto data=nlohmann::json{{"type",type},{"native",fields==nullptr}};
    if(fields) {data["values"]=nlohmann::json::parse(*fields,nullptr,false);if(!data["values"].is_object())data["values"]=nlohmann::json::object();data["metadata"]=nlohmann::json::parse(metadata);}
    const std::string payload=data.dump();Scripting::NativeRequest request;request.Entity=entt::to_integral(entity);request.Text=payload.c_str();
    int windows=0,widgets=0,groups=0;
    ImGui::PushID(("custom:"+type).c_str());
    const bool handled=Scripting::DrawEditorInspector(world,assets,request,[&](int op,Scripting::NativeRequest& r) {
        return ManagedEditorService(world,assets,op,r,windows,widgets,groups,true);
    });
    while(groups>0){ImGui::TreePop();--groups;}
    while(windows>0){ImGui::End();--windows;}
    ImGui::PopID();
    if(handled && fields && request.Text)*fields=request.Text;
    return handled;
}
