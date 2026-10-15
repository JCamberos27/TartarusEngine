#include "ProjectComponentMigration.h"
#include "ComponentRegistry.h"
#include "Log.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Scripting {
namespace {
using Json=nlohmann::json;

// The C# definition classes that are native components again, and the component each one became.
struct Definition { const char* Class; const char* Component; };
constexpr Definition kDefinitions[]={
    {"Tartarus.Gameplay.PlayerDefinition","First Person Controller"},
    {"Tartarus.Gameplay.NpcDefinition","NPC Spawn"},
    {"Tartarus.Gameplay.SquadDefinition","Squad Settings"},
    {"Tartarus.Gameplay.EffectsDefinition","FX & HUD Settings"},
    {"Tartarus.Gameplay.WeaponAudioDefinition","Weapon Audio"},
    {"Tartarus.Gameplay.FoleyDefinition","Foley Audio"},
    {"Tartarus.Gameplay.ImpactAudioDefinition","Impact Audio"},
    {"Tartarus.Gameplay.WeaponDefinition","Weapon Definition"},
    {"Tartarus.Gameplay.MuzzleAttachment","Muzzle Attachment"},
    {"Tartarus.Gameplay.GripAttachment","Grip Attachment"},
    {"Tartarus.Gameplay.OpticAttachment","Optic Attachment"},
};
// C# field names that are not the native label with its spaces taken out.
constexpr std::pair<const char*,const char*> kRenamed[]={
    {"KillY","Kill Height"},{"StickLookDegPerSec","Stick Look Deg/Sec"},
    {"CoverSpacing","Cover Sample Spacing"},{"CoverKneeHeight","Low Cover Height"},
    {"CoverHeadHeight","High Cover Height"},{"CoverStep","Cover Peek Step"},
    {"EnvTailGainOutdoorOpen","Env Gain Outdoor Open"},{"EnvTailGainOutdoorUrban","Env Gain Outdoor Urban"},
    {"EnvTailGainIndoorSmall","Env Gain Indoor Small"},{"EnvTailGainIndoorLarge","Env Gain Indoor Large"},
};

const Definition* Find(const std::string& className) {
    for(const auto& d:kDefinitions)if(className==d.Class)return &d;
    return nullptr;
}
// "Bleed-Out Time", "Volume Jitter dB" and "BleedOutTime", "VolumeJitterDb" all compare as one name.
std::string Folded(const char* name) {
    std::string out;
    for(const char* c=name;*c;++c)if(std::isalnum((unsigned char)*c))out+=(char)std::tolower((unsigned char)*c);
    return out;
}
const ReflectField* NativeField(const ReflectComponent& meta,const std::string& csharp) {
    std::string label=csharp;
    for(const auto& [from,to]:kRenamed)if(csharp==from)label=to;
    const std::string folded=Folded(label.c_str());
    for(const auto& f:meta.Fields)if(Folded(f.Name)==folded)return &f;
    return nullptr;
}
Json NativeValue(const ReflectField& f,const Json& value) {
    switch(f.Type) {
        case ReflectFieldType::Vec3:
        case ReflectFieldType::Color:
            if(value.is_object())return Json::array({value.value("X",0.0),value.value("Y",0.0),value.value("Z",0.0)});
            return value;
        case ReflectFieldType::String:
        case ReflectFieldType::AssetRef:
            if(value.is_object()) {
                // An AssetReference: keep the GUID where the native field tracks one.
                const auto path=value.value("path",std::string{});
                const auto guid=value.value("pathGuid",std::string{});
                if((f.AssetPath || f.Type==ReflectFieldType::AssetRef) && !guid.empty())return Json{{"path",path},{"pathGuid",guid}};
                return path;
            }
            return value;
        default:
            return value;
    }
}
// Adds the slot's fields to the entity's native block (the slot wins over a block already there).
void Convert(Json& entity,const Definition& definition,const Json& fields,bool enabled) {
    const RegisteredComponent* component=nullptr;
    for(const auto& rc:ComponentRegistry::All())if(std::strcmp(rc.Meta.Name,definition.Component)==0)component=&rc;
    if(!component)throw std::runtime_error(std::string("No native component for ")+definition.Class);
    const char* key=ReflectComponentKey(component->Meta);
    Json block=entity.value(key,Json::object());
    if(fields.is_object())for(const auto& [name,value]:fields.items()) {
        const ReflectField* f=NativeField(component->Meta,name);
        if(!f) {Log::Warn(std::string("Scene: ")+definition.Component+" has no field for the old C# value '"+name+"'; it is dropped.");continue;}
        block[ReflectFieldKey(*f)]=NativeValue(*f,value);
    }
    // A disabled attachment slot stays a disabled attachment; the other definitions have no off switch.
    if(!enabled) {
        if(NativeField(component->Meta,"Enabled"))block["Enabled"]=false;
        else Log::Warn(std::string("Scene: a disabled C# ")+definition.Class+" became an active "+definition.Component+".");
    }
    entity[key]=std::move(block);
}
}

bool ConvertScriptDefinitions(Json& entity) {
    const auto it=entity.find("C# Script");
    if(it==entity.end() || !it->is_object())return false;
    Json& script=*it;
    bool changed=false;
    if(const Definition* d=Find(script.value("Class",std::string{}))) {
        Convert(entity,*d,Json::parse(script.value("Fields JSON",std::string("{}"))),script.value("Enabled",true));
        for(const char* k:{"Class","Source","SourceGuid","Fields JSON","Enabled"})script.erase(k);
        changed=true;
    }
    auto extra=Json::parse(script.value("Scripts",std::string("[]")));
    if(!extra.is_array())throw std::runtime_error("C# Scripts must be an array");
    Json kept=Json::array();
    for(const auto& slot:extra) {
        if(const Definition* d=Find(slot.value("class",std::string{}))) {
            Convert(entity,*d,slot.value("fields",Json::object()),slot.value("enabled",true));changed=true;
        } else kept.push_back(slot);
    }
    if(!changed)return false;
    const bool primary=!script.value("Class",std::string{}).empty() || script.contains("Source");
    if(!primary && kept.empty())entity.erase("C# Script");
    else script["Scripts"]=kept.dump();
    return true;
}

bool MigrateLegacyProjectComponents(Json& entity) {
    if(!entity.contains("Health"))return false;
    const auto& legacy=entity.at("Health");Json fields=Json::object();
    for(const auto& [key,value]:legacy.items()) {
        std::string name=key;name.erase(std::remove(name.begin(),name.end(),' '),name.end());
        if(name.size()>4 && name.compare(name.size()-4,4,"Guid")==0)continue;
        fields[name]=value;
    }
    const std::string type="Tartarus.Gameplay.Health";
    const std::string source="assets/Scripts/Health.cs";
    auto script=entity.value("C# Script",Json::object());
    auto extra=Json::parse(script.value("Scripts",std::string("[]")));
    if(!extra.is_array())throw std::runtime_error("C# Scripts must be an array");
    bool found=false;
    auto merge=[&](Json& current) {for(const auto& [key,value]:fields.items())if(!current.contains(key))current[key]=value;found=true;};
    if(script.value("Class",std::string{})==type) {auto current=Json::parse(script.value("Fields JSON",std::string("{}")));merge(current);script["Fields JSON"]=current.dump();}
    for(auto& slot:extra)if(slot.value("class",std::string{})==type)merge(slot["fields"]);
    if(!found && !entity.contains("C# Script")) {
        script["Class"]=type;script["Source"]=source;script["Fields JSON"]=fields.dump();script["Enabled"]=true;
        script["Next Script ID"]=std::max(1,script.value("Next Script ID",0));
    } else if(!found) {
        int id=std::max(1,script.value("Next Script ID",1));
        for(const auto& slot:extra) {
            const int previous=slot.at("id").get<int>();
            if(previous<1 || previous>=std::numeric_limits<int>::max())throw std::runtime_error("Invalid C# script slot ID");
            id=std::max(id,previous+1);
        }
        if(id==std::numeric_limits<int>::max())throw std::runtime_error("Too many C# script slots");
        extra.push_back({{"id",id},{"source",source},{"class",type},{"fields",fields},{"enabled",true}});script["Next Script ID"]=id+1;
    }
    script["Scripts"]=extra.dump();entity["C# Script"]=std::move(script);entity.erase("Health");
    return true;
}
}
