#include "ScriptServices.h"
#include "ScriptComponent.h"
#include "World.h"
#include "ComponentRegistry.h"
#include "ProjectPaths.h"
#include "PhysicsWorld.h"
#include "GameModuleAPI.h"
#include "TimeService.h"
#include "AudioEngine.h"
#include "MaterialAsset.h"
#include <set>
#include <json.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace Scripting {
namespace {
using Json=nlohmann::json;
thread_local std::string response;
std::set<AudioEngine::SoundHandle> sounds;
int Reply(NativeRequest& r,const Json& value) { response=value.dump(); r.Text=response.c_str(); return 1; }
const RegisteredComponent* Find(const std::string& name) {
    for(const auto& c:ComponentRegistry::All()) if(name==c.Meta.Name) return &c;
    return nullptr;
}
Json Read(const ReflectField& f,void* p) {
    switch(f.Type) {
    case ReflectFieldType::Bool:return *static_cast<bool*>(p);
    case ReflectFieldType::Int:case ReflectFieldType::Enum:return *static_cast<int*>(p);
    case ReflectFieldType::Float:return *static_cast<float*>(p);
    case ReflectFieldType::Vec3:case ReflectFieldType::Color: {
        const auto& v=*static_cast<glm::vec3*>(p);return {{"X",v.x},{"Y",v.y},{"Z",v.z}};
    }
    default:return *static_cast<std::string*>(p);
    }
}
void Write(const ReflectField& f,void* p,const Json& v) {
    auto clamp=[&](float n) { if(!std::isfinite(n)) throw std::invalid_argument("Field must be finite"); return f.Min<f.Max?std::clamp(n,f.Min,f.Max):n; };
    switch(f.Type) {
    case ReflectFieldType::Bool:*static_cast<bool*>(p)=v.get<bool>();break;
    case ReflectFieldType::Int:case ReflectFieldType::Enum: {
        int n=v.get<int>();
        if(f.Type==ReflectFieldType::Enum && (n<0 || n>=f.EnumCount)) throw std::invalid_argument("Invalid enum index");
        if(f.Min<f.Max) n=static_cast<int>(std::clamp(static_cast<double>(n),static_cast<double>(f.Min),static_cast<double>(f.Max)));
        *static_cast<int*>(p)=n;break;
    }
    case ReflectFieldType::Float:*static_cast<float*>(p)=clamp(v.get<float>());break;
    case ReflectFieldType::Vec3:case ReflectFieldType::Color: {
        glm::vec3 value(clamp(v.at("X").get<float>()),clamp(v.at("Y").get<float>()),clamp(v.at("Z").get<float>()));
        *static_cast<glm::vec3*>(p)=value;break;
    }
    default:*static_cast<std::string*>(p)=v.get<std::string>();break;
    }
}
}
int PhysicsServices(int op,NativeRequest& r) {
    const std::string text=r.Text?r.Text:"";
    if(op==90 || op==91) {
        QueryFilter filter;filter.LayerMask=r.Script;filter.HitTriggers=r.Result?1:0;
        RaycastHit hit;
        const bool struck=op==90?PhysicsWorld::RaycastFiltered(&r.A.x,&r.B.x,r.Value,filter,hit)
            :PhysicsWorld::SphereCastFiltered(&r.A.x,&r.B.x,r.C.x,r.Value,filter,hit);
        if(!struck) return 0;
        r.Entity=hit.Entity;r.A={hit.Point[0],hit.Point[1],hit.Point[2]};r.B={hit.Normal[0],hit.Normal[1],hit.Normal[2]};r.Value=hit.Distance;return 1;
    }
    if(op==92) {
        const auto input=Json::parse(text);const int count=input.at("maxResults").get<int>();
        const float radius=input.at("radius").get<float>();
        if(count<1 || count>4096 || !std::isfinite(radius) || radius<0) throw std::invalid_argument("Invalid overlap bounds");
        float center[3]={input.at("x").get<float>(),input.at("y").get<float>(),input.at("z").get<float>()};
        QueryFilter filter;filter.LayerMask=input.at("layerMask").get<unsigned>();filter.HitTriggers=input.at("hitTriggers").get<bool>()?1:0;
        std::vector<unsigned> ids(static_cast<size_t>(count));const int found=PhysicsWorld::OverlapSphereFiltered(center,radius,filter,ids.data(),count);
        ids.resize(static_cast<size_t>(std::max(0,found)));return Reply(r,ids);
    }
    if(op!=58)return 0;
    static unsigned generation=0,owner=0xFFFFFFFFu;
    static std::uint64_t simulation=0;
    if(r.Result>=5 && r.Result<=7 && simulation!=PhysicsWorld::SimulationGeneration())return 0;
    switch(r.Result) {
    case 1: return PhysicsWorld::GetActorPosition(r.Entity,&r.A.x);
    case 2: { BodyState state;if(!PhysicsWorld::GetBodyState(r.Entity,state))return 0;r.Value=state.Kinematic?1.0f:0.0f;r.A={state.Velocity[0],state.Velocity[1],state.Velocity[2]};return state.Valid; }
    case 3: {float q[4];if(!PhysicsWorld::GetActorRotation(r.Entity,q))return 0;r.A={q[0],q[1],q[2]};r.Value=q[3];return 1;}
    case 4:
        if(PhysicsWorld::IsGrabbing())return 0;
        PhysicsWorld::GrabBody(r.Entity);if(PhysicsWorld::GrabbedEntity()!=r.Entity)return 0;
        owner=r.Entity;simulation=PhysicsWorld::SimulationGeneration();if(++generation==0)++generation;r.Script=generation;return 1;
    case 5: {
        if(r.Script!=generation || r.Entity!=owner || PhysicsWorld::GrabbedEntity()!=owner)return 0;
        const float q[4]={r.C.x,r.C.y,r.C.z,r.Value};PhysicsWorld::UpdateGrab(&r.A.x,&r.B.x,q);return 1;
    }
    case 6:
        if(r.Script!=generation || r.Entity!=owner || PhysicsWorld::GrabbedEntity()!=owner)return 0;
        PhysicsWorld::ReleaseBody(r.C.x!=0,&r.A.x,r.Value);owner=0xFFFFFFFFu;return 1;
    case 7:return r.Script==generation && r.Entity==owner && PhysicsWorld::GrabbedEntity()==owner;
    case 8: PhysicsWorld::AddForceAtPosition(r.Entity,&r.A.x,&r.B.x,ForceMode::Impulse);return 1;
    default:return 0;
    }
}
int ScriptServices(World& world,AssetLibrary*,int op,NativeRequest& r) {
    auto& registry=world.Registry;const auto e=static_cast<entt::entity>(r.Entity);
    const std::string text=r.Text?r.Text:"";
    if(op==93) return Reply(r,{{"time",Time::TimeSinceStartup()},{"unscaledTime",Time::UnscaledTimeSinceStartup()},
        {"realtime",Time::RealtimeSinceStartup()},{"frameCount",Time::FrameCount()},
        {"unscaledDeltaTime",Time::UnscaledDeltaTime()},{"timeScale",Time::TimeScale()}});
    if(op==94) {
        const auto input=Json::parse(text);AudioEngine::VoiceFx fx;
        fx.Spatial=input.at("spatial").get<bool>();
        fx.Position={input.at("x").get<float>(),input.at("y").get<float>(),input.at("z").get<float>()};
        fx.MinDistance=input.at("minDistance").get<float>();fx.MaxDistance=input.at("maxDistance").get<float>();
        const int bus=input.at("bus").get<int>();if(bus<0 || bus>4) return 0;
        const auto id=AudioEngine::Play(ProjectPaths::Resolve(input.at("path").get<std::string>()),input.at("volume").get<float>(),input.at("loop").get<bool>(),static_cast<AudioEngine::Bus>(bus),0,&fx);
        if(id) AudioEngine::SetPitch(id,input.value("pitch",1.0f));
        // Retain only live handles; loops are explicitly stopped on Play exit.
        for(auto it=sounds.begin();it!=sounds.end();) if(!AudioEngine::IsPlaying(*it)) it=sounds.erase(it);else ++it;
        if(id) sounds.insert(id);r.Entity=id;return id?1:0;
    }
    if(op==95) {AudioEngine::Stop(r.Entity);sounds.erase(r.Entity);return 1;}
    if(op==96) return AudioEngine::IsPlaying(r.Entity);
    if(op==97) {AudioEngine::SetVolume(r.Entity,r.Value);return 1;}
    if(op==98) {AudioEngine::SetPosition(r.Entity,{r.A.x,r.A.y,r.A.z});return 1;}
    if(op==60) return registry.valid(e);
    if(op==61) {
        for(auto [id,n]:registry.view<NameComponent>().each()) if(n.Name==text) {r.Entity=entt::to_integral(id);return 1;}
        return 0;
    }
    if(op==62) {r.Entity=entt::to_integral(world.CreateEmptyEntity({r.A.x,r.A.y,r.A.z},{0,0,0},{1,1,1},text));return 1;}
    if(op==68) {
        auto ids=Json::array();for(auto id:registry.view<TransformComponent>()) ids.push_back(entt::to_integral(id));return Reply(r,ids);
    }
    if(op==74) {
        auto catalog=Json::array();
        for(const auto& c:ComponentRegistry::All()) {
            auto fields=Json::array();for(const auto& f:c.Meta.Fields) {
                auto labels=Json::array();for(int i=0;i<f.EnumCount;++i) labels.push_back(ReflectEnumLabel(f,i));
                fields.push_back({{"key",ReflectFieldKey(f)},{"label",f.Name},{"kind",static_cast<int>(f.Type)},
                    {"min",f.Min},{"max",f.Max},{"tooltip",f.Tooltip?f.Tooltip:""},{"enumLabels",labels},
                    {"group",f.Group?f.Group:""},{"editorHidden",f.EditorHidden},
                    {"visibleIfField",f.VisibleIfField?f.VisibleIfField:""},{"visibleIfValue",f.VisibleIfValue},{"visibleIfNot",f.VisibleIfNot}});
            }
            catalog.push_back({{"name",c.Meta.Name},{"category",c.Meta.Category},{"fields",fields}});
        }
        return Reply(r,catalog);
    }
    if(op==77) return Reply(r,ProjectPaths::Resolve(text));
    if(op==78) return PhysicsWorld::IsActive();
    if(op==99 && r.Result==4) {r.Entity=PhysicsWorld::GrabbedEntity();return 1;}
    if(op==99 && r.Result==5) return Reply(r,PhysicsWorld::EventSequence());
    if(!registry.valid(e)) return 0;
    if(op==99) {
        if(r.Result==1) {const auto* tag=registry.try_get<TagComponent>(e);return Reply(r,tag?tag->Tag:"");}
        if(r.Result==2) {
            auto* render=registry.try_get<RenderableComponent>(e);
            if(!render || r.Script>=render->Materials.size() || !render->Materials[r.Script] || !std::isfinite(r.Value)) return 0;
            render->Materials[r.Script]->Mat.EmissiveStrength=std::max(0.0f,r.Value);return 1;
        }
        if(r.Result==3) return PhysicsWorld::GetActorPosition(r.Entity,&r.A.x);
    }
    if(op>=81 && op<=84) {
        const auto* a=registry.try_get<AnimatorControllerComponent>(e);if(!a) return 0;
        if(op==81) return Reply(r,a->CurrentState());
        if(op==82) return a->HasTag(text);
        if(op==83) return a->EventFired(text);
        return a->InState(text);
    }
    if(op==87 || op==88) {
        if(!registry.all_of<TransformComponent>(e)) return 0;
        glm::mat4 matrix=world.ComposeWorldTransform(e);
        if(op==88) {if(std::abs(glm::determinant(matrix))<1e-8f) return 0;matrix=glm::inverse(matrix);}
        const glm::vec3 point(matrix*glm::vec4(r.A.x,r.A.y,r.A.z,1));r.A={point.x,point.y,point.z};return 1;
    }
    if(op==79) {
        auto& c=registry.get_or_emplace<CSharpScriptComponent>(e);Attach(c,"",text);r.Script=GetSlots(c).back().Id;return 1;
    }
    if(op==80) {
        auto* c=registry.try_get<CSharpScriptComponent>(e);if(!c) return 0;
        auto slots=GetSlots(*c);const auto found=std::find_if(slots.begin(),slots.end(),[&](const ScriptSlot& s){return s.Id==r.Script;});
        if(found==slots.end()) return 0;slots.erase(found);SetSlots(*c,slots);return 1;
    }
    if(op==63) {auto* n=registry.try_get<NameComponent>(e);return Reply(r,n?n->Name:"");}
    if(op==64) {registry.get_or_emplace<NameComponent>(e).Name=text;return 1;}
    if(op==65) {const auto* h=registry.try_get<HierarchyComponent>(e);r.Entity=h?entt::to_integral(h->Parent):0xFFFFFFFFu;return 1;}
    if(op==66) return world.SetParent(e,static_cast<entt::entity>(r.Script));
    if(op==67) {
        auto ids=Json::array();if(auto* h=registry.try_get<HierarchyComponent>(e)) for(auto child:h->Children) if(registry.valid(child)) ids.push_back(entt::to_integral(child));return Reply(r,ids);
    }
    if(op==69) {auto names=Json::array();for(const auto& c:ComponentRegistry::All()) if(c.Has(registry,e)) names.push_back(c.Meta.Name);return Reply(r,names);}
    if((op>=70 && op<=73) || op==75) {
        const auto input=Json::parse(text);const auto* c=Find(input.at("component").get<std::string>());
        if(!c) return 0;
        if(op==75) return c->Has(registry,e);
        // Simulated actor and renderer ownership can't be safely rebuilt from a data API.
        // Author structural native components in Edit mode, then enter Play to activate them.
        if(op==72 || op==73) {
            if(PhysicsWorld::IsActive() || std::string(c->Meta.Name)=="C# Script" || std::string(c->Meta.Name)=="Mesh Renderer") return 0;
            if(op==72) {if(!c->Has(registry,e)) c->Add(registry,e);} else if(c->Has(registry,e)) c->Remove(registry,e);
            return 1;
        }
        void* component=c->Get(registry,e);if(!component) return 0;
        const auto key=input.at("field").get<std::string>();
        for(const auto& f:c->Meta.Fields) if(key==ReflectFieldKey(f) || key==f.Name) {
            if(op==70) return Reply(r,Read(f,f.Address(component)));
            if(std::string(c->Meta.Name)=="C# Script") return 0; // use stable script-slot APIs
            Write(f,f.Address(component),input.at("value"));return 1;
        }
    }
    return 0;
}
void StopScriptSounds() {for(auto id:sounds) AudioEngine::Stop(id);sounds.clear();}
}
