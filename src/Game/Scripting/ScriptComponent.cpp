#include "ScriptComponent.h"
#include "Components.h"
#include <json.hpp>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <stdexcept>
#include <limits>
namespace Scripting {
namespace {
std::vector<ScriptSlot> ParseExtraSlots(const std::string& scripts) {
    std::vector<ScriptSlot> slots;
    const auto extra=nlohmann::json::parse(scripts);
    if(!extra.is_array()) throw std::runtime_error("C# Scripts must be an array");
    std::set<std::uint32_t> ids{0};
    for(const auto& value:extra) {
        ScriptSlot slot; slot.Id=value.at("id").get<std::uint32_t>();
        if(slot.Id>=static_cast<std::uint32_t>(std::numeric_limits<int>::max())) throw std::runtime_error("Invalid C# script slot ID");
        if(!ids.insert(slot.Id).second) throw std::runtime_error("Duplicate C# script slot ID");
        slot.Source=value.value("source",std::string{}); slot.Class=value.value("class",std::string{});
        slot.Fields=value.value("fields",nlohmann::json::object()).dump(); slot.Enabled=value.value("enabled",true);
        slots.push_back(std::move(slot));
    }
    return slots;
}
// The script runtime asks for every component's slots several times per tick; parsing the Scripts JSON each
// time was ~5% of a Play frame. Parsed results are kept per Scripts text (per thread; bounded).
const std::vector<ScriptSlot>& ExtraSlotsCached(const std::string& scripts) {
    thread_local std::unordered_map<std::string,std::vector<ScriptSlot>> cache;
    if(auto it=cache.find(scripts); it!=cache.end()) return it->second;
    auto slots=ParseExtraSlots(scripts); // throws before anything is cached
    if(cache.size()>=256) cache.clear();
    return cache.emplace(scripts,std::move(slots)).first->second;
}
}
std::vector<ScriptSlot> GetSlots(const CSharpScriptComponent& component) {
    std::vector<ScriptSlot> slots;
    if(!component.ClassName.empty() || !component.SourcePath.empty())
        slots.push_back({0,component.SourcePath,component.ClassName,component.Fields,component.Enabled});
    const auto& extra=ExtraSlotsCached(component.Scripts);
    slots.insert(slots.end(),extra.begin(),extra.end());
    return slots;
}
bool SlotEnabled(const CSharpScriptComponent& component,std::uint32_t id,bool& enabled) {
    if(id==0 && (!component.ClassName.empty() || !component.SourcePath.empty())) { enabled=component.Enabled; return true; }
    for(const auto& slot:ExtraSlotsCached(component.Scripts)) if(slot.Id==id) { enabled=slot.Enabled; return true; }
    return false;
}
void SetSlots(CSharpScriptComponent& component,const std::vector<ScriptSlot>& slots) {
    for(const auto& old:GetSlots(component)) component.NextScriptId=std::max(component.NextScriptId,static_cast<int>(old.Id)+1);
    component.SourcePath.clear(); component.ClassName.clear(); component.Fields="{}"; component.Enabled=true;
    auto extra=nlohmann::json::array();
    for(const auto& slot:slots) {
        component.NextScriptId=std::max(component.NextScriptId,static_cast<int>(slot.Id)+1);
        if(slot.Id==0) {
            component.SourcePath=slot.Source; component.ClassName=slot.Class;
            component.Fields=slot.Fields; component.Enabled=slot.Enabled;
        } else extra.push_back({{"id",slot.Id},{"source",slot.Source},{"class",slot.Class},
                              {"fields",nlohmann::json::parse(slot.Fields)},{"enabled",slot.Enabled}});
    }
    component.Scripts=extra.dump();
}
void Attach(CSharpScriptComponent& component,const std::string& source,const std::string& className) {
    auto slots=GetSlots(component);
    std::uint32_t id=static_cast<std::uint32_t>(component.NextScriptId);
    for(const auto& slot:slots) id=std::max(id,slot.Id+1);
    if(id>=static_cast<std::uint32_t>(std::numeric_limits<int>::max())) throw std::runtime_error("Too many C# script slots");
    slots.push_back({id,source,className,"{}",true}); SetSlots(component,slots);
}
}
