#include "ParticleSystem.h"
#include "Components.h"
#include "ProjectSettings.h"
#include "World.h"
#include "PhysicsWorld.h"
#include "GameModuleAPI.h"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace {
float Rand01(std::uint32_t& s) {
    if(!s)s=0x9E3779B9u;
    s^=s<<13;s^=s>>17;s^=s<<5;
    return float(s&0xFFFFFFu)/float(0x1000000u);
}
glm::vec3 InCone(std::uint32_t& s,float angle) {
    const float y=glm::mix(1.0f,std::cos(angle),Rand01(s));
    const float r=std::sqrt(std::max(0.0f,1.0f-y*y)),phi=Rand01(s)*glm::two_pi<float>();
    return {r*std::cos(phi),y,r*std::sin(phi)};
}
glm::mat3 Basis(const glm::mat4& m) {
    glm::mat3 b(1.0f);
    for(int i=0;i<3;++i)if(glm::length(glm::vec3(m[i]))>1e-6f)b[i]=glm::normalize(glm::vec3(m[i]));
    return b;
}
void Curves(ParticleSystemComponent& ps) {
    const std::string stamp=ps.SizeCurve+'|'+ps.AlphaCurve+'|'+ps.SpeedCurve+'|'+ps.EmissionCurve;
    if(!ps.Curves || ps.Curves.use_count()!=1)ps.Curves=std::make_shared<ParticleCurveCache>();
    if(stamp==ps.Curves->Stamp)return;
    ps.Curves->Stamp=stamp;
    auto parse=[](const std::string& text,Curve& out) {
        Curve c;if(Curve::FromJson(nlohmann::json::parse(text,nullptr,false),c)&&!c.Empty())out=std::move(c);
        else out=Curve::Constant(1.0f);
    };
    parse(ps.SizeCurve,ps.Curves->Size);parse(ps.AlphaCurve,ps.Curves->Alpha);
    parse(ps.SpeedCurve,ps.Curves->Speed);parse(ps.EmissionCurve,ps.Curves->Emission);
}
void Spawn(World& world,entt::entity entity,ParticleSystemComponent& ps,int count,float subFrame) {
    const int cap=std::clamp(ps.MaxParticles,0,100000);
    count=std::clamp(count,0,std::max(0,cap-int(ps.Live.size())));
    if(!count||ps.Lifetime<=0)return;
    const glm::mat4 m=world.ComposeWorldTransform(entity);const auto basis=Basis(m);
    const float radius=std::max(0.0f,ps.ShapeRadius);
    auto range=[&](float a,float b){return glm::mix(std::min(a,b),std::max(a,b),Rand01(ps.Rng));};
    for(int i=0;i<count;++i) {
        ParticleSystemComponent::Particle p;
        glm::vec3 local(0),direction=InCone(ps.Rng,glm::radians(std::clamp(ps.Spread,0.0f,180.0f)));
        const float phi=Rand01(ps.Rng)*glm::two_pi<float>();
        if(ps.Shape==1) {
            const float h=std::cbrt(Rand01(ps.Rng))*std::max(0.0f,ps.ShapeLength);
            const float r=radius*(ps.ShapeLength>1e-6f?h/ps.ShapeLength:1.0f)*(ps.ShapeSurface?1.0f:std::sqrt(Rand01(ps.Rng)));
            local={r*std::cos(phi),h,r*std::sin(phi)};
        } else if(ps.Shape==2) {
            direction=InCone(ps.Rng,glm::pi<float>());
            local=direction*radius*(ps.ShapeSurface?1.0f:std::cbrt(Rand01(ps.Rng)));
        } else if(ps.Shape==3) {
            const glm::vec3 box=glm::max(ps.ShapeBox,glm::vec3(0));
            local=glm::vec3(range(-.5f,.5f),range(-.5f,.5f),range(-.5f,.5f))*box;
            if(ps.ShapeSurface) {
                const glm::vec3 area(box.y*box.z,box.x*box.z,box.x*box.y);
                const float pick=Rand01(ps.Rng)*(area.x+area.y+area.z);
                const int face=pick<area.x?0:pick<area.x+area.y?1:2;
                local[face]=(Rand01(ps.Rng)<.5f?-.5f:.5f)*box[face];
            }
        } else if(ps.Shape==4) {
            const float r=radius*(ps.ShapeSurface?1.0f:std::sqrt(Rand01(ps.Rng)));
            local={r*std::cos(phi),0,r*std::sin(phi)};
        }
        const float speed=std::max(0.0f,ps.StartSpeed)*(1+range(-1,1)*std::clamp(ps.SpeedRandomness,0.0f,1.0f));
        p.Local=ps.LocalSpace;
        p.Pos=p.Local?local:glm::vec3(m*glm::vec4(local,1));
        p.Vel=(p.Local?direction:basis*direction)*speed;
        p.Life=std::max(.001f,ps.Lifetime*(1+range(-1,1)*std::clamp(ps.LifetimeRandomness,0.0f,1.0f)));
        p.SizeScale=std::max(0.0f,1+range(-1,1)*std::clamp(ps.SizeRandomness,0.0f,1.0f));
        p.Rotation=glm::radians(range(ps.RotationMin,ps.RotationMax));
        p.Spin=glm::radians(range(ps.AngularVelocityMin,ps.AngularVelocityMax));
        const int cells=std::clamp(ps.SheetColumns,1,256)*std::clamp(ps.SheetRows,1,256);
        p.FirstFrame=ps.RandomStartFrame?std::min(cells-1,int(Rand01(ps.Rng)*cells)):0;
        p.Seed=Rand01(ps.Rng);p.Age=Rand01(ps.Rng)*std::max(0.0f,subFrame);
        p.Pos+=p.Vel*p.Age;ps.Live.push_back(std::move(p));
    }
}
}
void RestartParticleSystem(ParticleSystemComponent& ps,bool clear) {
    if(clear)ps.Live.clear();
    ps.Clock=ps.EmitAccumulator=ps.DistanceAccumulator=0;ps.WasEmitting=false;ps.HavePreviousPosition=false;
}
void EmitParticleBurst(World& world,entt::entity entity,int count) {
    if(!world.Registry.valid(entity)||world.Registry.all_of<InactiveTag>(entity)||!world.Registry.all_of<TransformComponent>(entity))return;
    if(auto* ps=world.Registry.try_get<ParticleSystemComponent>(entity))Spawn(world,entity,*ps,count,0);
}
void UpdateParticleSystems(World& world,float dt) {
    if(!std::isfinite(dt)||dt<=0)return;dt=std::min(dt,.1f);
    for(auto [e,ps]:world.Registry.view<ParticleSystemComponent>().each()) {
        if(world.Registry.all_of<InactiveTag>(e)||!world.Registry.all_of<TransformComponent>(e)) {
            RestartParticleSystem(ps);continue;
        }
        Curves(ps);
        const glm::mat4 m=world.ComposeWorldTransform(e);
        const bool invertible=std::abs(glm::determinant(glm::mat3(m)))>1e-8f;
        const glm::mat4 inv=invertible?glm::inverse(m):glm::mat4(1);
        const glm::vec3 origin(m[3]);
        const glm::vec3 gravity=ProjectSettings::Physics().Gravity*ps.GravityModifier;
        for(size_t i=0;i<ps.Live.size();) {
            auto& p=ps.Live[i];p.Age+=dt;
            if(p.Age>=p.Life) {p=std::move(ps.Live.back());ps.Live.pop_back();continue;}
            const float t=std::clamp(p.Age/std::max(.001f,p.Life),0.0f,1.0f);
            const glm::vec3 g=p.Local?glm::mat3(inv)*gravity:gravity;
            p.Vel=(p.Vel+(g+ps.Acceleration)*dt)*std::exp(-std::max(0.0f,ps.Drag)*dt);
            const glm::vec3 old=p.Pos;
            p.Pos+=(p.Vel*std::max(0.0f,ps.Curves->Speed.Evaluate(t))+ps.VelocityOverLife)*dt;
            p.Rotation+=p.Spin*dt;
            if(ps.Collision && (!p.Local||invertible)) {
                const glm::vec3 from=p.Local?glm::vec3(m*glm::vec4(old,1)):old;
                glm::vec3 to=p.Local?glm::vec3(m*glm::vec4(p.Pos,1)):p.Pos;
                glm::vec3 normal(0,1,0),contact(0);bool hit=false;
                const float skin=std::max(0.0f,ps.CollisionRadius);
                if(ps.Collision==1 && to.y<ps.CollisionPlaneY+skin && to.y<=from.y) {
                    const float fraction=std::clamp((from.y-ps.CollisionPlaneY-skin)/std::max(1e-6f,from.y-to.y),0.0f,1.0f);
                    contact=glm::mix(from,to,fraction);contact.y=ps.CollisionPlaneY+skin;hit=true;
                } else if(ps.Collision==2 && PhysicsWorld::IsActive()) {
                    const glm::vec3 delta=to-from;const float distance=glm::length(delta);
                    if(distance>1e-6f) {
                        const glm::vec3 dir=delta/distance;RaycastHit result;QueryFilter filter;filter.HitTriggers=0;
                        const float o[3]={from.x,from.y,from.z},d[3]={dir.x,dir.y,dir.z};
                        if(PhysicsWorld::RaycastFiltered(o,d,distance+skin,filter,result)&&result.Entity!=entt::to_integral(e)) {
                            normal={result.Normal[0],result.Normal[1],result.Normal[2]};
                            contact=glm::vec3(result.Point[0],result.Point[1],result.Point[2])+normal*skin;hit=true;
                        }
                    }
                }
                if(hit) {
                    glm::vec3 velocity=p.Local?glm::mat3(m)*p.Vel:p.Vel;
                    const float vn=glm::dot(velocity,normal);
                    if(vn<0)velocity=(velocity-normal*vn)*(1-std::clamp(ps.CollisionFriction,0.0f,1.0f))-normal*vn*std::clamp(ps.Bounce,0.0f,1.0f);
                    p.Vel=p.Local?glm::mat3(inv)*velocity:velocity;
                    p.Pos=p.Local?glm::vec3(inv*glm::vec4(contact,1)):contact;
                    p.Age+=p.Life*std::clamp(ps.CollisionLifeLoss,0.0f,1.0f);
                    if(++p.Bounces>std::max(0,ps.MaxBounces))p.Age=p.Life;
                }
            }
            ++i;
        }
        const bool first=ps.Emitting&&!ps.WasEmitting;
        if(first)RestartParticleSystem(ps,false);
        ps.WasEmitting=ps.Emitting;
        if(!ps.Emitting) {ps.EmitAccumulator=ps.DistanceAccumulator=0;ps.PreviousPosition=origin;ps.HavePreviousPosition=true;continue;}
        const float before=ps.Clock;ps.Clock+=dt;
        const float delay=std::max(0.0f,ps.StartDelay),duration=std::max(.01f,ps.Duration);
        const float begin=std::max(before,delay),end=ps.Looping?ps.Clock:std::min(ps.Clock,delay+duration);
        if(end>=begin && ps.Clock>=delay && (ps.Looping||before<delay+duration)) {
            const float phase=std::fmod(std::max(0.0f,(begin+end)*.5f-delay),duration)/duration;
            ps.EmitAccumulator+=std::max(0.0f,ps.Rate)*std::max(0.0f,ps.Curves->Emission.Evaluate(phase))*std::max(0.0f,end-begin);
            if(ps.HavePreviousPosition && end>begin)ps.DistanceAccumulator+=glm::length(origin-ps.PreviousPosition)*std::max(0.0f,ps.RateOverDistance)*(end-begin)/dt;
            const int count=int(std::min(100000.0f,std::floor(ps.EmitAccumulator)))+int(std::min(100000.0f,std::floor(ps.DistanceAccumulator)));
            ps.EmitAccumulator-=std::floor(ps.EmitAccumulator);ps.DistanceAccumulator-=std::floor(ps.DistanceAccumulator);
            Spawn(world,e,ps,count,dt);
            if(ps.BurstCount>0) {
                const int cycle0=std::max(0,int(std::floor((begin-delay)/duration)));
                const int cycle1=ps.Looping?std::max(cycle0,int(std::floor((end-delay)/duration))):0;
                int events=0;
                for(int cycle=cycle0;cycle<=cycle1 && events<1024;++cycle) {
                    const float start=delay+cycle*duration;
                    const float interval=ps.BurstInterval>0?std::max(.001f,ps.BurstInterval):duration;
                    const int event0=std::max(0,int(std::ceil((begin-start)/interval)));
                    for(int event=event0;events<1024;++event) {
                        const float at=start+event*interval;
                        if(at>end||at>=start+duration)break;
                        if(at>before || (first && at==before))++events;
                    }
                }
                Spawn(world,e,ps,int(std::min(100000LL,static_cast<long long>(events)*std::clamp(ps.BurstCount,0,100000))),0);
            }
        }
        ps.PreviousPosition=origin;ps.HavePreviousPosition=true;
    }
}
