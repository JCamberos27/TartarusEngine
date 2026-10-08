#include "ProjectComponentMigration.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Scripting {
bool MigrateLegacyProjectComponents(nlohmann::json& entity) {
    using Json=nlohmann::json;
    bool changed=false;
    for(const auto& pair:{std::pair{"Goal Trigger","BasketGoal"},std::pair{"Scoreboard","Scoreboard"},std::pair{"Score Digit","ScoreDigit"},std::pair{"Impact Sound","ImpactSound"},std::pair{"First Person Controller","PlayerDefinition"},std::pair{"Impact Audio","ImpactAudioDefinition"},std::pair{"Weapon Audio","WeaponAudioDefinition"},std::pair{"Foley Audio","FoleyDefinition"},std::pair{"FX & HUD Settings","EffectsDefinition"},std::pair{"Health","Health"},std::pair{"NPC Spawn","NpcDefinition"},std::pair{"Squad Settings","SquadDefinition"}}) {
        if(!entity.contains(pair.first))continue;
        const auto& legacy=entity.at(pair.first);Json fields=Json::object();
        for(const auto& [key,value]:legacy.items()) {
            std::string name=key;name.erase(std::remove(name.begin(),name.end(),' '),name.end());
            if(name.size()>4 && name.compare(name.size()-4,4,"Guid")==0)continue;
            if(name=="VolumeJitterdB")name="VolumeJitterDb";
            if(name=="EnvGainOutdoorOpen")name="EnvTailGainOutdoorOpen";
            if(name=="EnvGainOutdoorUrban")name="EnvTailGainOutdoorUrban";
            if(name=="EnvGainIndoorSmall")name="EnvTailGainIndoorSmall";
            if(name=="EnvGainIndoorLarge")name="EnvTailGainIndoorLarge";
            if(name=="VolumeJitterdB")name="VolumeJitterDb";
            if(name=="NPCStepVolume")name="NpcStepVolume";
            if(name=="NPCStepMinDistance")name="NpcStepMinDistance";
            if(name=="NPCStepMaxDistance")name="NpcStepMaxDistance";
            if(name=="KillHeight")name="KillY";
            if(name=="StickLookDeg/Sec")name="StickLookDegPerSec";
            if(name=="ViewModelFOV")name="ViewModelFov";
            if(name=="FieldofView")name="FieldOfView";
            if(name=="NPCDamageScale")name="NpcDamageScale";
            if(name=="CoverSampleSpacing")name="CoverSpacing";
            if(name=="LowCoverHeight")name="CoverKneeHeight";
            if(name=="HighCoverHeight")name="CoverHeadHeight";
            if(name=="CoverPeekStep")name="CoverStep";
            if(name=="DamageScale")name="NpcDamageScale";
            if((name=="Weapon" || name=="Brain") && value.is_string() && std::string(pair.first)=="NPC Spawn") {
                fields[name]=name=="Weapon"?(value=="Remington 870"?1:value=="Random"?2:0):(value=="Training Dummy"?1:0);continue;
            }
            if((name=="Team" || name=="Place") && value.is_string()) {
                fields[name]=(value=="Away" || value=="Tens")?1:0;continue;
            }
            if((name=="DataFile" || name=="Clip" || name=="ScoreSound" || name=="PrimaryWeaponPrefab" || name=="SecondaryWeaponPrefab" || name=="AnimationSet" || name=="SecondaryAnimationSet") && value.is_string()) {
                const auto guid=legacy.value(key+"Guid",std::string{});
                fields[name]={{"path",value},{"pathGuid",guid}};
            } else if((name=="ViewModelOffset" || name=="ViewModelRotation") && value.is_array())fields[name]={{"X",value.at(0)},{"Y",value.at(1)},{"Z",value.at(2)}};
            else fields[name]=value;
        }
        const auto type=std::string("Tartarus.Gameplay.")+pair.second;
        const auto source=std::string("assets/Scripts/")+pair.second+".cs";
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
        script["Scripts"]=extra.dump();entity["C# Script"]=std::move(script);entity.erase(pair.first);changed=true;
    }
    return changed;
}
}
