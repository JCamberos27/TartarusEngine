#include "EditorTestProbe.h" // --editor-tests widget names
#include "EditorLayer.h"
#include "ScriptIDE.h"
#include "ProjectPaths.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "ComponentRegistry.h"
#include "World.h"
#include "Scripting/ScriptRuntime.h"
#include "Scripting/ScriptReferences.h"
#include "ScriptReferenceWidgets.h"
#include "AssetPathPicker.h"
#include "AssetDatabase.h"
#include "EditorTheme.h"
#include "EditorUIPrimitives.h"
#include <imgui.h>
#include <imgui_internal.h> // DC.IsSameLine
#include <json.hpp>
#include <cfloat>
#include <cstring>
using namespace EditorInternal;
bool DrawScriptReferenceField(World& world, entt::entity owner, const nlohmann::json& metadata,
                              nlohmann::json& value, GLFWwindow* window) {
    const std::string kind = metadata.at("kind");
    const bool asset=kind=="asset-ref";
    std::string reference=asset && value.is_object()?
        AssetDatabase::FollowRef(value.value("path",std::string{}),value.value("pathGuid",std::string{})):
        value.is_null()?std::string{}:value.get<std::string>();
    bool changed = false;
    if (asset) {
        const auto extensions=metadata.value("extensions",std::vector<std::string>{});
        std::vector<const char*> extensionPointers;
        for(const auto& extension:extensions)extensionPointers.push_back(extension.c_str());
        extensionPointers.push_back(nullptr);
        AssetPathPickerOptions options;options.Extensions=extensions.empty()?nullptr:extensionPointers.data();
        options.DragPayload="ASSET_FILE_PATH";options.Owner=window;
        changed=AssetPathPicker("##asset",reference,options);
    } else if (kind == "scene-ref") {
        const std::string component = metadata.value("component", std::string("Transform"));
        const bool childrenOnly = metadata.value("childrenOnly", false);
        const auto target = Scripting::ResolveSceneReference(world, owner, reference);
        const bool valid = Scripting::AcceptsSceneReference(world, owner, target, component, childrenOnly);
        const auto* name = valid ? world.Registry.try_get<NameComponent>(target) : nullptr;
        const std::string preview = reference.empty() ? "None (" + component + ")" :
            valid ? (name ? name->Name : reference) + " (" + component + ")" : "Missing: " + reference;
        const bool open = ImGui::BeginCombo("##reference", preview.c_str(), ImGuiComboFlags_HeightLarge);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s\nDrag a %s from the Hierarchy.", reference.c_str(), component.c_str());
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
                if (payload->DataSize == sizeof(entt::entity)) {
                    const auto dropped = *static_cast<const entt::entity*>(payload->Data);
                    if (Scripting::AcceptsSceneReference(world, owner, dropped, component, childrenOnly)) {
                        reference = *Scripting::SceneReferencePath(world, owner, dropped); changed = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (open) {
            if (ImGui::Selectable(("None (" + component + ")").c_str(), reference.empty())) { reference.clear(); changed = true; }
            static char filter[128] = {};
            if (ImGui::IsWindowAppearing()) filter[0] = '\0';
            ImGui::InputTextWithHint("##filter", "Find object", filter, sizeof(filter));
            std::vector<std::pair<std::string, std::string>> choices;
            for (auto e : world.Registry.view<TransformComponent>()) {
                if (!Scripting::AcceptsSceneReference(world, owner, e, component, childrenOnly)) continue;
                const auto path = *Scripting::SceneReferencePath(world, owner, e);
                if (*filter && path.find(filter) == std::string::npos) continue;
                const auto* objectName = world.Registry.try_get<NameComponent>(e);
                choices.emplace_back(path, path == "." && objectName ? objectName->Name + " (self)" : path);
            }
            std::sort(choices.begin(), choices.end());
            for (const auto& choice : choices) if (ImGui::Selectable(choice.second.c_str(), choice.first == reference)) {
                reference = choice.first; changed = true;
            }
            ImGui::EndCombo();
        }
    } else if (kind == "sound-refs") {
        std::vector<std::string> clips;
        size_t begin = 0;
        while (begin < reference.size()) {
            const auto end = reference.find(';', begin);
            auto clip = reference.substr(begin, end == std::string::npos ? end : end - begin);
            if (!clip.empty()) clips.push_back(std::move(clip));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        // A trailing empty slot accepts the next asset without requiring text or separators.
        clips.emplace_back();
        AssetPathPickerOptions options;
        options.Extensions = AssetExts::Sounds; options.DragPayload = "ASSET_SOUND_PATH";
        options.DialogFilter = "Audio\0*.wav;*.mp3;*.ogg;*.flac\0"; options.Owner = window;
        for (size_t i = 0; i < clips.size(); ++i) {
            ImGui::PushID(static_cast<int>(i)); ImGui::SetNextItemWidth(-FLT_MIN);
            options.NoneLabel = i + 1 == clips.size() ? "Add firing sound (Audio Clip)" : "None (remove clip)";
            changed |= AssetPathPicker("##sound", clips[i], options);
            ImGui::PopID();
        }
        if (changed) {
            reference.clear();
            for (const auto& clip : clips) if (!clip.empty()) {
                if (!reference.empty()) reference += ';';
                reference += clip;
            }
        }
    } else if (kind == "choice") {
        if (ImGui::BeginCombo("##choice", reference.empty() ? "(weapon default)" : reference.c_str())) {
            for (const auto& choice : metadata.at("choices")) {
                const std::string entry = choice;
                if (ImGui::Selectable(entry.empty() ? "(weapon default)" : entry.c_str(), reference == entry)) {
                    reference = entry; changed = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    if (changed) {
        if(asset)value={{"path",reference},{"pathGuid",reference.empty()?std::string{}:AssetDatabase::GuidForPath(ProjectPaths::Resolve(reference)).ToString()}};
        else value=reference;
    }
    return changed;
}
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
        // A C# foldout: the framed raised strip every Inspector foldout uses.
        const bool open=EditorUIPrimitives::FramedFoldout(text,r.Result?ImGuiTreeNodeFlags_DefaultOpen:0);
        if(open)++groups;return open?1:0;
    }
    if(op==118) {if(!groups)return 0;ImGui::TreePop();--groups;return 1;}
    // vInspector: greyed-out ([ReadOnly] / [DisableIf]) fields and inline rows (variant chips).
    if(op==130) {if(r.Result)ImGui::BeginDisabled(true);else ImGui::EndDisabled();return 1;}
    if(op==131) {ImGui::SameLine();return 1;}
    if(op==122) {if(ImGui::IsItemHovered())EditorUI::SetTooltip("%s",text);return 1;}
    ImGui::PushID(++widgets);
    struct Pop {~Pop(){ImGui::PopID();}} pop;
    // --editor-tests: name the widget a C# inspector draws after its label ("cs:Max Speed"), and a
    // text field after its value too, so a read-out can be checked ("cs:Doubled##show=8").
    struct Tag {
        int op; const char* label; Scripting::NativeRequest& r;
        ~Tag() {
            if(!EditorTestProbeActive() || op<103 || op>121) return;
            std::string name=label;
            if(op==115) {
                try { name=nlohmann::json::parse(label).value("label",std::string{})+"="+(r.Text?r.Text:""); } catch(...) {}
            } else if(op==121) {
                try { name=nlohmann::json::parse(label).value("label",std::string{}); } catch(...) {}
            }
            EditorTestTag(("cs:"+name).c_str());
        }
    } tag{op,text,r};
    switch(op) {
    case 102:ImGui::TextUnformatted(text);return 1;
    case 103:{
        // Buttons and variant chips: the raised secondary button, so neither reads as bare text.
        const bool chip=ImGui::GetCurrentWindow()->DC.IsSameLine || std::strstr(text,"##")!=nullptr;
        if(chip) EditorTheme::PushSmall();
        const bool clicked=EditorUIPrimitives::SecondaryButton(text);
        if(chip) EditorTheme::PopFont();
        return clicked?1:0;
    }
    case 104:{bool value=r.Result!=0;const bool changed=EditorUIPrimitives::Checkbox(text,&value);r.Result=value?1:0;return changed?1:0;}
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
              // A toolbar of choices: the segmented look (accent wash on the chosen one).
              ImGui::PushStyleColor(ImGuiCol_Button,active?EditorTheme::AccentWash:EditorTheme::Raised);
              ImGui::PushStyleColor(ImGuiCol_ButtonHovered,active?EditorTheme::WithAlpha(EditorTheme::Accent,0.20f):EditorTheme::Hover);
              ImGui::PushStyleColor(ImGuiCol_ButtonActive,active?EditorTheme::WithAlpha(EditorTheme::Accent,0.28f):EditorTheme::Pressed);
              ImGui::PushStyleColor(ImGuiCol_Text,active?EditorTheme::AccentBright:EditorTheme::Secondary);
              if(ImGui::Button(labels[i].c_str()))r.Result=static_cast<int>(i);
              ImGui::PopStyleColor(4);
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
    case 132:{
        const auto data=nlohmann::json::parse(text);static thread_local std::string result;
        const auto label=data.at("label").get<std::string>();auto value=data.at("value");
        ImGui::TextUnformatted(label.c_str());ImGui::SetNextItemWidth(-FLT_MIN);
        const bool changed=DrawScriptReferenceField(world,static_cast<entt::entity>(r.Entity),data.at("metadata"),value,m_Window);
        if(changed) {PushUndo(world,"Set " + label);m_Dirty=true;}
        result=data.at("metadata").at("kind")=="asset-ref"?value.dump():value.get<std::string>();r.Text=result.c_str();return changed?1:0;
    }
    case 133:return EditorUI::ColorEditLinear(text,&r.A.x,ImGuiColorEditFlags_DisplayHex)?1:0;
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
                                     std::string* fields,const std::string& metadata,std::uint32_t slot) {
    auto data=nlohmann::json{{"type",type},{"native",fields==nullptr},{"slot",slot}};
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
    if(handled && fields && request.Text) {
        const auto before=nlohmann::json::parse(*fields,nullptr,false);
        const auto after=nlohmann::json::parse(request.Text);
        if(before!=after) {
            // The result is still a detached slot copy: capture its authored state before SetSlots.
            // Explicit C# Undo.RecordScene labels win; ordinary custom fields get automatic history.
            BeginSceneUndo(world,m_GlobalUndoActive?m_GlobalUndoLabel:"Edit C# Script");
            *fields=after.dump();
        }
    }
    return handled;
}
