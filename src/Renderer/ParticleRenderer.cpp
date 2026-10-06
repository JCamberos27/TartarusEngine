#include "ParticleRenderer.h"
#include "Components.h"
#include "ParticleSystem.h"
#include "GLStateCache.h"
#include "RenderFrameContext.h"
#include "Shader.h"
#include "ProjectPaths.h"
#include "ShaderLibrary.h"
#include "Texture.h"
#include "World.h"
#include "gl.h"
#include <algorithm>
#include <cmath>
#include <vector>

ParticleRenderer::~ParticleRenderer() {
    delete m_Shader;
    if(m_Vbo)glDeleteBuffers(1,&m_Vbo);
    if(m_Vao)glDeleteVertexArrays(1,&m_Vao);
}
namespace {
struct Instance {
    float Pos[3],Size,Color[4],Axis[4],Params[4],Atlas[4];
};
struct Item {Instance Data;Texture* Tex=nullptr;int Blend=0;float Depth=0;};
struct Batch {Texture* Tex;int Blend;GLuint First;GLsizei Count;};
static_assert(sizeof(Instance)==80);
}
void ParticleRenderer::EnsureCreated() {
    if(m_Shader)return;
    m_Shader=new Shader(ShaderLibrary::ReadFile("Particle.vert.glsl"),ShaderLibrary::ReadFile("Particle.frag.glsl"));
    glCreateVertexArrays(1,&m_Vao);glCreateBuffers(1,&m_Vbo);
    glVertexArrayVertexBuffer(m_Vao,0,m_Vbo,0,sizeof(Instance));glVertexArrayBindingDivisor(m_Vao,0,1);
    for(unsigned i=0;i<5;++i) {
        glEnableVertexArrayAttrib(m_Vao,i);glVertexArrayAttribFormat(m_Vao,i,4,GL_FLOAT,GL_FALSE,sizeof(float)*4*i);
        glVertexArrayAttribBinding(m_Vao,i,0);
    }
}
Texture* ParticleRenderer::ParticleTexture(const std::string& path,bool flame) {
    const std::string key=(flame?"data:":"color:")+path;
    auto it=m_Textures.find(key);
    if(it==m_Textures.end()) {
        TextureImportSettings s;s.IsSRGB=!flame;s.WrapMode=TextureImportSettings::Wrap::ClampToEdge;
        auto tex=std::make_shared<Texture>(ProjectPaths::Resolve(path),s);
        it=m_Textures.emplace(key,std::move(tex)).first;
    }
    return it->second->IsValid()?it->second.get():nullptr;
}
int ParticleRenderer::Draw(const World& world,const RenderFrameContext& ctx,bool viewModelPass,bool drawViewModel) {
    static std::vector<Item> items;static std::vector<Instance> upload;static std::vector<Batch> batches;
    items.clear();upload.clear();batches.clear();
    for(auto [e,ps]:world.Registry.view<const ParticleSystemComponent>().each()) {
        if(ps.Live.empty()||world.Registry.all_of<InactiveTag>(e))continue;
        if(ctx.EditorView&&world.Registry.all_of<HiddenInSceneTag>(e))continue;
        if(ctx.OwnerView?world.Registry.all_of<HiddenFromOwnerTag>(e):world.Registry.all_of<OwnerViewOnlyTag>(e))continue;
        if((viewModelPass&&world.Registry.all_of<ViewModelTag>(e))!=drawViewModel)continue;
        const glm::mat4 transform=world.ComposeWorldTransform(e);
        const int columns=std::clamp(ps.SheetColumns,1,256),rows=std::clamp(ps.SheetRows,1,256),cells=columns*rows;
        for(const auto& p:ps.Live) {
            if(p.Age>=p.Life)continue;
            const float t=std::clamp(p.Age/std::max(p.Life,1e-4f),0.0f,1.0f);
            const glm::vec3 pos=p.Local?glm::vec3(transform*glm::vec4(p.Pos,1)):p.Pos;
            const glm::vec3 vel=p.Local?glm::mat3(transform)*p.Vel:p.Vel;
            const std::string& path=p.TextureOverride.empty()?ps.Texture:p.TextureOverride;
            const bool flame=p.Length>0 && !path.empty();
            Texture* tex=path.empty()?nullptr:ParticleTexture(path,flame);
            Instance in{};in.Pos[0]=pos.x;in.Pos[1]=pos.y;in.Pos[2]=pos.z;
            int blend=ps.BlendMode==1?1:0;
            if(flame&&tex) {
                const float across=t*(2.9f+t*(-2.8f+t*.9f));
                const float along=t*(1.69f+t*(-.38f-t*.31f));
                const float hot=t<.1824f?glm::mix(1.0f,.03124f,t/.1824f):std::max(0.0f,glm::mix(.03124f,0.0f,(t-.1824f)/(.3618f-.1824f)));
                const float a=p.Alpha*(t<.55f?glm::mix(1.0f,.294f,t/.55f):glm::mix(.294f,0.0f,(t-.55f)/.45f));
                const glm::vec3 c=ps.StartColor*p.Tint*(std::max(ps.Intensity,0.0f)*p.IntensityScale*hot*p.Glow);
                if(a<=0||t<=0)continue;
                in.Size=p.Width*across;
                in.Color[0]=c.r;in.Color[1]=c.g;in.Color[2]=c.b;in.Color[3]=a;
                in.Axis[0]=p.Axis.x;in.Axis[1]=p.Axis.y;in.Axis[2]=p.Axis.z;in.Axis[3]=p.Length*along;
                in.Params[2]=p.Seed;in.Params[3]=1;blend=2; // premultiplied flame
            } else {
                const glm::vec3 c=glm::mix(ps.StartColor,ps.EndColor,t)*p.Tint*(std::max(ps.Intensity,0.0f)*p.IntensityScale);
                const float a=std::clamp(glm::mix(ps.StartAlpha,ps.EndAlpha,t)*p.Alpha*(ps.Curves?ps.Curves->Alpha.Evaluate(t):1.0f),0.0f,1.0f);
                in.Size=std::max(0.0f,glm::mix(ps.StartSize,ps.EndSize,t)*p.SizeScale*(ps.Curves?ps.Curves->Size.Evaluate(t):1.0f));
                if(a<=0||in.Size<=0)continue;
                in.Color[0]=c.r;in.Color[1]=c.g;in.Color[2]=c.b;in.Color[3]=a;
                in.Params[0]=p.Rotation;
                const float frame=std::max(0.0f,ps.SheetFPS>0?p.Age*ps.SheetFPS:t*float(cells-1));
                const int step=int(std::floor(frame));
                const int aFrame=(p.FirstFrame+step)%cells;
                const int bFrame=ps.SheetFPS>0?(aFrame+1)%cells:(p.FirstFrame+std::min(step+1,cells-1))%cells;
                in.Params[1]=ps.BlendFrames?frame-std::floor(frame):0;
                in.Params[3]=ps.Alignment==2?3.0f:0.0f;
                in.Atlas[0]=float(columns);in.Atlas[1]=float(rows);in.Atlas[2]=float(aFrame);in.Atlas[3]=float(bFrame);
                if(ps.Alignment==1&&glm::length(vel)>1e-6f) {
                    const glm::vec3 axis=glm::normalize(vel);
                    in.Axis[0]=axis.x;in.Axis[1]=axis.y;in.Axis[2]=axis.z;
                    in.Axis[3]=in.Size+glm::length(vel)*std::max(0.0f,ps.VelocityStretch);in.Params[3]=2;
                }
            }
            const float depth=-(ctx.View*glm::vec4(pos,1)).z;
            items.push_back({in,tex,blend,depth});
        }
    }
    if(items.empty())return 0;
    EnsureCreated();
    // Alpha particles stay globally back-to-front even when their textures differ.
    // Additive particles can group by texture; flames are premultiplied and sorted with alpha.
    std::stable_sort(items.begin(),items.end(),[](const Item& a,const Item& b) {
        const bool aa=a.Blend==1,bb=b.Blend==1;
        if(aa!=bb)return !aa;
        if(!aa)return a.Depth>b.Depth;
        return std::less<Texture*>{}(a.Tex,b.Tex);
    });
    upload.reserve(items.size());
    for(const auto& item:items) {
        if(batches.empty()||batches.back().Tex!=item.Tex||batches.back().Blend!=item.Blend)
            batches.push_back({item.Tex,item.Blend,GLuint(upload.size()),0});
        ++batches.back().Count;upload.push_back(item.Data);
    }
    if(upload.size()>m_Capacity) {
        m_Capacity=std::max(upload.size(),m_Capacity*2);
        glNamedBufferData(m_Vbo,GLsizeiptr(m_Capacity*sizeof(Instance)),nullptr,GL_DYNAMIC_DRAW);
    }
    glNamedBufferSubData(m_Vbo,0,GLsizeiptr(upload.size()*sizeof(Instance)),upload.data());
    const GLboolean prevBlend=glIsEnabled(GL_BLEND),prevCull=glIsEnabled(GL_CULL_FACE),prevDepth=glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevMask;glGetBooleanv(GL_DEPTH_WRITEMASK,&prevMask);
    glEnable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDisable(GL_CULL_FACE);glEnable(GL_BLEND);
    m_Shader->Bind();m_Shader->SetMat4("uView",ctx.View);m_Shader->SetMat4("uProj",ctx.Proj);
    m_Shader->SetInt("uParticleTex",0);glBindVertexArray(m_Vao);
    for(const auto& batch:batches) {
        if(batch.Blend==2)glBlendFuncSeparate(GL_ONE,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
        else if(batch.Blend==1)glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE,GL_ZERO,GL_ONE);
        else glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
        m_Shader->SetInt("uTextured",batch.Tex?1:0);if(batch.Tex)batch.Tex->Bind(0);
        glDrawArraysInstancedBaseInstance(GL_TRIANGLES,0,6,batch.Count,batch.First);
    }
    glBindVertexArray(0);glDepthMask(prevMask);
    if(!prevDepth)glDisable(GL_DEPTH_TEST);
    if(!prevBlend)glDisable(GL_BLEND);
    if(prevCull)glEnable(GL_CULL_FACE);
    GLStateCache::Invalidate();return int(items.size());
}
