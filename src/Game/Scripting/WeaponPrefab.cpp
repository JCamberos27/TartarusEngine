#include "WeaponPrefab.h"
#include "ProjectPaths.h"
#include "AssetDatabase.h"
#include "Components.h"
#include "ComponentRegistry.h"
#include <json.hpp>
#include <fstream>

namespace Scripting {
bool ResolveWeaponPrefab(const std::string& prefab,std::string& animationSet,std::string& error,
                         WeaponDefinitionComponent* definition) {
    try {
        std::ifstream file(ProjectPaths::Resolve(prefab));
        if(!file) { error="Cannot open weapon prefab: "+prefab; return false; }
        const auto json=nlohmann::json::parse(file);
        // The scene serializer uses `empties` for non-rendered roots.
        for(const char* collection:{"empties","models","boxes"}) {
            if(!json.contains(collection)) continue;
            for(const auto& entity:json.at(collection)) {
                if(!entity.contains("Weapon Definition")) continue;
                const auto& value=entity.at("Weapon Definition").at("Animation Set");
                animationSet=value.is_string()?value.get<std::string>():value.value("path",std::string{});
                if(value.is_object()) animationSet=AssetDatabase::FollowRef(animationSet,value.value("pathGuid",std::string{}));
                if(animationSet.empty()) { error="Weapon prefab has no Animation Set: "+prefab; return false; }
                if(definition) {
                    WeaponDefinitionComponent loaded;
                    const auto& fields=entity.at("Weapon Definition");
                    for(const auto& rc:ComponentRegistry::All()) if(std::string(rc.Meta.Name)=="Weapon Definition")
                        for(const auto& f:rc.Meta.Fields) {
                            const auto it=fields.find(ReflectFieldKey(f));if(it==fields.end())continue;
                            void* p=f.Address(&loaded);
                            switch(f.Type) {
                            case ReflectFieldType::Bool:*static_cast<bool*>(p)=it->get<bool>();break;
                            case ReflectFieldType::Int:case ReflectFieldType::Enum:*static_cast<int*>(p)=it->get<int>();break;
                            case ReflectFieldType::Float:*static_cast<float*>(p)=it->get<float>();break;
                            case ReflectFieldType::Color:case ReflectFieldType::Vec3:
                                *static_cast<glm::vec3*>(p)={it->at(0).get<float>(),it->at(1).get<float>(),it->at(2).get<float>()};break;
                            case ReflectFieldType::String:case ReflectFieldType::AssetRef:
                                *static_cast<std::string*>(p)=it->is_string()?it->get<std::string>():
                                    AssetDatabase::FollowRef(it->value("path",std::string{}),it->value("pathGuid",std::string{}));break;
                            }
                        }
                    loaded.AnimationSet=animationSet;*definition=std::move(loaded);
                }
                return true;
            }
        }
        error="Prefab has no Weapon Definition: "+prefab;
    } catch(const std::exception& e) { error="Invalid weapon prefab '"+prefab+"': "+e.what(); }
    return false;
}
}
