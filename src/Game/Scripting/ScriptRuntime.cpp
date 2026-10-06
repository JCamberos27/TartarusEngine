#include "ScriptRuntime.h"
#include "ScriptComponent.h"
#include "ScriptServices.h"
#include "World.h"
#include "AssetLibrary.h"
#include "SceneSerializer.h"
#include "PhysicsWorld.h"
#include "InputMap.h"
#include "ProjectPaths.h"
#include "EnginePaths.h"
#include "Log.h"
#include "GameModuleAPI.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <array>
#include <algorithm>
#include <chrono>
#include <map>
#include <json.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace Scripting {
namespace {
using Dispatch = int (__cdecl*)(int, void*, int, void*);
Dispatch dispatch = nullptr;
bool attemptedLoad = false;
std::string error;
World* activeWorld = nullptr;
AssetLibrary* activeAssets = nullptr;
const std::function<void(const NativeRequest&)>* shotTrace = nullptr;
std::set<std::uint64_t> live;
std::set<std::string> rejectedScripts;
HANDLE buildProcess = nullptr;
std::filesystem::file_time_type assemblyTime{};
bool buildPending=false;
std::chrono::steady_clock::time_point buildAfter;
std::string description;
std::map<std::string,std::string> descriptionCache;
const std::function<int(int,NativeRequest&)>* editorServices=nullptr;
std::filesystem::file_time_type editorAssemblyTime{};
bool editorAttempted=false;
bool editorBuild=false;
bool editorBuildNext=false;
std::filesystem::path ModuleDir() {
    wchar_t file[32768]{}; GetModuleFileNameW(nullptr,file,32768);
    return std::filesystem::path(file).parent_path() / "Managed";
}
std::filesystem::path GameplayPath() {
    auto project = std::filesystem::u8path(ProjectPaths::Resolve("Scripts/bin/Tartarus.Gameplay.dll"));
    auto engine=ModuleDir()/"Tartarus.Gameplay.dll";
    std::error_code ec;
    if(!std::filesystem::exists(project,ec)) return engine;
    const auto projectTime=std::filesystem::last_write_time(project,ec);
    const auto engineTime=std::filesystem::last_write_time(engine,ec);
    return ec || projectTime>engineTime ? project : engine;
}
Vec3 Pack(const glm::vec3& v) { return {v.x,v.y,v.z}; }
glm::vec3 Unpack(const Vec3& v) { return {v.x,v.y,v.z}; }
int NativeImpl(int op, NativeRequest* r) {
    if (!r) return 0;
    if(op>=100) return editorServices?(*editorServices)(op,*r):0;
    // Physics services retain ownership of PhysX objects; no native pointer escapes into a script.
    switch(op) {
    case 0:
        if(r->Text && std::string(r->Text).rfind("C# error:",0)==0) Log::Error(r->Text);
        else Log::Info(r->Text ? r->Text : "");
        return 1;
    case 30: description=r->Text?r->Text:"{}"; return 1;
    case 1: return PhysicsWorld::IsActive();
    case 2: return PhysicsWorld::HasCharacter();
    case 3: PhysicsWorld::CreateCharacter(r->B.x,r->B.y,&r->A.x); return PhysicsWorld::HasCharacter();
    case 4: PhysicsWorld::GetCharacterFootPosition(&r->A.x); return 1;
    case 5: PhysicsWorld::SetCharacterFootPosition(&r->A.x); return 1;
    case 6: return PhysicsWorld::ResizeCharacter(r->Value);
    case 7: return PhysicsWorld::CharacterFitsAt(r->Value);
    case 8: return static_cast<int>(PhysicsWorld::MoveCharacter(&r->A.x,r->Value));
    case 9: r->Value=PhysicsWorld::PlatformYawDelta(); return 1;
    case 10: r->Value=InputMap::GetAxis(r->Text?r->Text:""); return 1;
    case 11: return InputMap::GetButton(r->Text?r->Text:"");
    case 12: return InputMap::GetButtonDown(r->Text?r->Text:"");
    case 13: return InputMap::GetButtonUp(r->Text?r->Text:"");
    case 25: case 26: {
        RaycastHit hit; QueryFilter filter; filter.HitTriggers=0;
        const bool recording=PhysicsWorld::GetQueryRecording();
        if(op==26) PhysicsWorld::SetQueryRecording(false);
        const bool struck=PhysicsWorld::RaycastFiltered(&r->A.x,&r->B.x,r->Value,filter,hit);
        if(op==26) PhysicsWorld::SetQueryRecording(recording);
        if (!struck) return 0;
        r->A={hit.Point[0],hit.Point[1],hit.Point[2]}; r->B={hit.Normal[0],hit.Normal[1],hit.Normal[2]};
        r->Entity=hit.Entity; r->Value=hit.Distance; return hit.Hit;
    }
    default: break;
    }
    if(op==29 && shotTrace) { (*shotTrace)(*r); return 1; }
    if (!activeWorld) return 0;
    if(op>=60 && op<=99) return ScriptServices(*activeWorld,activeAssets,op,*r);
    auto& registry=activeWorld->Registry;
    const auto e=static_cast<entt::entity>(r->Entity);
    if (op==22 && activeAssets && r->Text) {
        const auto root=SceneSerializer::InstantiatePrefab(*activeWorld,*activeAssets,ProjectPaths::Resolve(r->Text));
        if(root==entt::null) return 0;
        if(auto* t=registry.try_get<TransformComponent>(root)) t->Position=Unpack(r->A);
        r->Entity=static_cast<std::uint32_t>(root); return 1;
    }
    if(!registry.valid(e)) return 0;
    if(op==40) {
        switch(r->Result) {
        case 0: return registry.all_of<TransformComponent>(e);
        case 1: return registry.all_of<RigidbodyComponent>(e);
        case 2: return registry.all_of<AnimatorControllerComponent>(e);
        default: return 0;
        }
    }
    if(op>=31 && op<=36) {
        auto* t=registry.try_get<TransformComponent>(e); if(!t) return 0;
        if(op==31) r->A=Pack(t->Scale);
        if(op==32) t->Scale=Unpack(r->A);
        if(op==33) { r->A={t->Rotation.x,t->Rotation.y,t->Rotation.z}; r->Value=t->Rotation.w; }
        if(op==34) {
            glm::quat q(r->Value,r->A.x,r->A.y,r->A.z);
            if(glm::length(q)<1e-6f) return 0;
            t->SetRotationQuaternion(glm::normalize(q));
        }
        if(op==35) r->A=Pack(glm::vec3(activeWorld->ComposeWorldTransform(e)[3]));
        if(op==36) {
            glm::mat4 parent(1);
            if(const auto* p=registry.try_get<HierarchyComponent>(e); p && registry.valid(p->Parent)) parent=activeWorld->ComposeWorldTransform(p->Parent);
            if(std::abs(glm::determinant(parent))<1e-8f) return 0;
            t->Position=glm::vec3(glm::inverse(parent)*glm::vec4(Unpack(r->A),1));
        }
        return 1;
    }
    if(op>=41 && op<=44) {
        const auto* body=registry.try_get<RigidbodyComponent>(e); if(!body) return 0;
        if(op==43) { r->Value=body->Mass; return 1; }
        BodyState state; if(!PhysicsWorld::GetBodyState(r->Entity,state) || body->IsKinematic) return 0;
        if(op==41) r->A={state.Velocity[0],state.Velocity[1],state.Velocity[2]};
        if(op==42) PhysicsWorld::SetLinearVelocity(r->Entity,&r->A.x);
        if(op==44) PhysicsWorld::AddForce(r->Entity,&r->A.x,static_cast<unsigned>(r->Result));
        return 1;
    }
    if(op==39 || (op>=45 && op<=48)) {
        auto* a=registry.try_get<AnimatorControllerComponent>(e); if(!a || !r->Text) return 0;
        if(op==39) a->ResetTrigger(r->Text);
        if(op==45) a->SetFloat(r->Text,r->Value);
        if(op==46) a->SetInt(r->Text,static_cast<int>(r->Value));
        if(op==47) a->SetBool(r->Text,r->Value!=0);
        if(op==48) r->Value=a->GetFloat(r->Text);
        return 1;
    }
    if(op==49) return !registry.all_of<DeactivatedTag>(e);
    if(op==50) {
        if(r->Result) registry.remove<DeactivatedTag>(e); else registry.get_or_emplace<DeactivatedTag>(e);
        activeWorld->SyncActiveInHierarchy(); return 1;
    }
    if(op==51) {
        auto* c=registry.try_get<CSharpScriptComponent>(e); if(!c) return 0;
        auto slots=GetSlots(*c);
        for(auto& slot:slots) if(slot.Id==r->Script) { slot.Enabled=r->Result!=0; SetSlots(*c,slots); return 1; }
        return 0;
    }
    if(op==27 || op==28) {
        const auto* body=registry.try_get<RigidbodyComponent>(e);
        if(!body || body->IsKinematic) return 0;
        if(op==27) r->Value=body->Mass;
        else PhysicsWorld::AddForceAtPosition(r->Entity,&r->A.x,&r->B.x,ForceMode::Impulse);
        return 1;
    }
    if(op==20 || op==21) {
        auto* t=registry.try_get<TransformComponent>(e); if(!t) return 0;
        if(op==20) r->A=Pack(t->Position); else t->Position=Unpack(r->A);
        return 1;
    }
    if(op==23) { activeWorld->DestroyEntityAndChildren(e); return 1; }
    if(op==24) {
        auto* a=registry.try_get<AnimatorControllerComponent>(e); if(!a||!r->Text) return 0;
        a->SetTrigger(r->Text); return 1;
    }
    return 0;
}
int Native(int op,NativeRequest* r) {
    try {return NativeImpl(op,r);} catch(const std::exception& why) {Log::Error(std::string("C# native service: ")+why.what());return 0;}
}
bool Fail(const std::string& message) { error=message; Log::Error("C# runtime: "+message); return false; }
std::filesystem::path FindHostFxr() {
    std::vector<std::filesystem::path> roots;
    for (const wchar_t* key : {L"DOTNET_ROOT_X64", L"DOTNET_ROOT", L"ProgramFiles"}) {
        wchar_t value[32768]{};
        if(GetEnvironmentVariableW(key,value,32768)) roots.emplace_back(std::filesystem::path(value)/(std::wstring(key)==L"ProgramFiles"?L"dotnet":L""));
    }
    for(const auto& root:roots) {
        std::error_code ec; std::filesystem::path best; std::array<int,3> bestVersion{};
        for(const auto& entry:std::filesystem::directory_iterator(root/"host/fxr",ec)) {
            const auto name=entry.path().filename().string(); std::array<int,3> v{};
            if(sscanf_s(name.c_str(),"%d.%d.%d",&v[0],&v[1],&v[2])==3 && v>=bestVersion && std::filesystem::exists(entry.path()/"hostfxr.dll")) { best=entry.path()/"hostfxr.dll"; bestVersion=v; }
        }
        if(!best.empty()) return best;
    }
    return {};
}
}
const std::string& LastError() { return error; }
bool EnsureLoaded() {
    if(dispatch) return true;
    if(attemptedLoad) return false;
    attemptedLoad=true;
    const auto fxr=FindHostFxr();
    if(fxr.empty()) return Fail(".NET 10 runtime was not found. Install the x64 .NET 10 runtime (SDK for editing scripts).");
    HMODULE host=LoadLibraryW(fxr.c_str()); if(!host) return Fail("Could not load hostfxr.dll");
    using Init=int(__cdecl*)(const wchar_t*,const void*,void**);
    using Delegate=int(__cdecl*)(void*,int,void**);
    using Close=int(__cdecl*)(void*);
    using Load=int(__stdcall*)(const wchar_t*,const wchar_t*,const wchar_t*,const wchar_t*,void*,void**);
    auto init=reinterpret_cast<Init>(GetProcAddress(host,"hostfxr_initialize_for_runtime_config"));
    auto get=reinterpret_cast<Delegate>(GetProcAddress(host,"hostfxr_get_runtime_delegate"));
    auto close=reinterpret_cast<Close>(GetProcAddress(host,"hostfxr_close"));
    if(!init||!get||!close) return Fail("hostfxr exports are missing");
    void* context=nullptr; const auto dir=ModuleDir();
    const int initialized=init((dir/"Tartarus.Runtime.runtimeconfig.json").c_str(),nullptr,&context);
    if(initialized<0||!context) return Fail("Could not initialize .NET from "+dir.string());
    void* loader=nullptr; int rc=get(context,5,&loader); close(context);
    if(rc<0||!loader) return Fail("Could not acquire .NET assembly loader");
    void* entry=nullptr;
    rc=reinterpret_cast<Load>(loader)((dir/"Tartarus.Runtime.dll").c_str(),L"Tartarus.Entry, Tartarus.Runtime",L"Dispatch",reinterpret_cast<const wchar_t*>(-1),nullptr,&entry);
    if(rc<0||!entry) return Fail("Could not load the managed scripting entry point");
    auto candidate=reinterpret_cast<Dispatch>(entry);
    const auto path=GameplayPath(); const std::string utf8=path.u8string();
    if(candidate(0,const_cast<char*>(utf8.c_str()),kVersion,reinterpret_cast<void*>(&Native))!=0) return Fail("Gameplay assembly initialization failed");
    dispatch=candidate; std::error_code ec; assemblyTime=std::filesystem::last_write_time(path,ec); error.clear(); return true;
}
bool Invoke(int op, void* frame, int size) {
    if(!EnsureLoaded()) return false;
    const bool ok=dispatch(op,frame,size,reinterpret_cast<void*>(&Native))==0;
    if(ok && (op==0 || op==5)) { descriptionCache.clear(); rejectedScripts.clear(); }
    return ok;
}
bool InvokeShot(World& world, ShotFrame& frame, const std::function<void(const NativeRequest&)>& trace) {
    World* previousWorld=activeWorld;
    const auto* previousTrace=shotTrace;
    activeWorld=&world; shotTrace=&trace;
    const bool ok=Invoke(6,&frame,sizeof frame);
    activeWorld=previousWorld; shotTrace=previousTrace;
    return ok;
}
void Tick(World& world, AssetLibrary& assets, float dt, bool fixed) {
    activeWorld=&world; activeAssets=&assets;
    Poll();
    std::set<std::uint64_t> current;
    struct Binding { entt::entity Entity; ScriptSlot Slot; };
    std::vector<Binding> bindings;
    // Snapshot bindings: callbacks can spawn, disable or destroy entities and script slots.
    for(auto [e,s]:world.Registry.view<CSharpScriptComponent>().each()) {
        try { for(auto& slot:GetSlots(s)) if(!slot.Class.empty()) {
            current.insert((static_cast<std::uint64_t>(slot.Id)<<32)|static_cast<std::uint32_t>(e));
            bindings.push_back({e,std::move(slot)});
        } } catch(const std::exception& why) {
            const std::string key=std::to_string(static_cast<std::uint32_t>(e))+s.Scripts;
            if(rejectedScripts.insert(key).second) Fail(why.what());
        }
    }
    auto active=[&](const Binding& b) {
        if(!world.Registry.valid(b.Entity) || world.Registry.all_of<InactiveTag>(b.Entity)) return false;
        const auto* component=world.Registry.try_get<CSharpScriptComponent>(b.Entity); if(!component) return false;
        for(const auto& slot:GetSlots(*component)) if(slot.Id==b.Slot.Id) return slot.Enabled;
        return false;
    };
    auto send=[&](const Binding& b,int phase) {
        if(!world.Registry.valid(b.Entity) || !world.Registry.all_of<CSharpScriptComponent>(b.Entity)) return;
        const auto id=static_cast<std::uint32_t>(b.Entity);
        const std::string key=std::to_string(id)+":"+std::to_string(b.Slot.Id)+b.Slot.Class+b.Slot.Fields;
        if(rejectedScripts.count(key)) return;
        EntityFrame f; f.Dt=dt; f.Entity=id; f.Script=b.Slot.Id; f.Phase=phase;
        f.ClassName=b.Slot.Class.c_str(); f.Fields=b.Slot.Fields.c_str();
        // Component.enabled stays true when only its GameObject is inactive.
        if(const auto* c=world.Registry.try_get<CSharpScriptComponent>(b.Entity))
            for(const auto& slot:GetSlots(*c)) if(slot.Id==b.Slot.Id) f.Enabled=slot.Enabled;
        if(!Invoke(3,&f,sizeof f)) rejectedScripts.insert(key);
    };
    for(auto id:live) if(!current.count(id)) {
        EntityFrame f; f.Entity=static_cast<std::uint32_t>(id); f.Script=static_cast<std::uint32_t>(id>>32);
        f.Phase=3; Invoke(3,&f,sizeof f);
    }
    for(const auto& b:bindings) send(b,5); // construct all scripts before Awake/OnEnable
    for(const auto& b:bindings) send(b,active(b)?0:4);
    for(const auto& b:bindings) if(active(b)) send(b,fixed?2:1);
    if(!fixed) for(const auto& b:bindings) if(active(b)) send(b,6);
    live=std::move(current); activeWorld=nullptr; activeAssets=nullptr;
}
void Stop(World* world, AssetLibrary* assets) {
    // OnDestroy must have the same world services as Update.
    activeWorld=world; activeAssets=assets;
    if(dispatch) Invoke(4,nullptr,0);
    StopScriptSounds();
    live.clear(); rejectedScripts.clear(); activeWorld=nullptr; activeAssets=nullptr;
}
bool Building() { return buildProcess!=nullptr; }
bool BuildPending() { return buildPending; }
void RequestBuild() { buildPending=true; buildAfter=std::chrono::steady_clock::now()+std::chrono::milliseconds(600); }
std::string Describe(const std::string& className) {
    if(auto it=descriptionCache.find(className); it!=descriptionCache.end()) return it->second;
    NativeRequest request; request.Text=className.c_str(); description.clear();
    if(!Invoke(7,&request,sizeof request)) description=nlohmann::json{{"error","Script type unavailable. Save the source to compile it, or use File > Build C# Gameplay."}}.dump();
    descriptionCache[className]=description; return description;
}
bool Build() {
    if(buildProcess) return false;
    buildPending=false;
    auto project=std::filesystem::u8path(ProjectPaths::Resolve("Scripts/Tartarus.Gameplay.csproj"));
    if(!std::filesystem::exists(project)) {
        // Bootstrap a project's gameplay once; preserve every source file the user already authored.
        std::error_code ec;
        const auto sources=std::filesystem::u8path(ProjectPaths::Resolve("assets/Scripts"));
        std::filesystem::create_directories(project.parent_path(),ec);
        if(!ec) std::filesystem::create_directories(sources,ec);
        const auto templates=ModuleDir()/"SDK/Templates";
        for(const char* name:{"Gameplay.cs","PlayerController.cs","WeaponController.cs"}) {
            if(ec) break;
            std::filesystem::copy_file(templates/name,sources/name,std::filesystem::copy_options::skip_existing,ec);
        }
        if(!ec) std::filesystem::copy_file(templates/"Tartarus.Gameplay.csproj",project,std::filesystem::copy_options::skip_existing,ec);
        if(ec) return Fail("Cannot create C# gameplay project: "+ec.message());
    }
    // Bootstrap editor compilation only when editor source exists; never replace an authored project.
    {
        std::error_code ec;
        const auto templates=ModuleDir()/"SDK/Templates";
        const auto props=project.parent_path()/"Directory.Build.props";
        if(!std::filesystem::exists(props)) std::filesystem::copy_file(templates/"Directory.Build.props",props,std::filesystem::copy_options::skip_existing,ec);
        const auto editorProject=project.parent_path()/"Tartarus.Editor.csproj";
        if(std::filesystem::exists(std::filesystem::u8path(ProjectPaths::Resolve("assets/Editor"))) && !std::filesystem::exists(editorProject))
            std::filesystem::copy_file(templates/"Tartarus.Editor.csproj",editorProject,std::filesystem::copy_options::skip_existing,ec);
        if(ec) return Fail("Cannot create C# editor project: "+ec.message());
    }
    const auto output=project.parent_path()/"bin";
    editorBuild=false;
    editorBuildNext=std::filesystem::exists(project.parent_path()/"Tartarus.Editor.csproj");
    // A quoted executable path and fixed SDK arguments; project paths never become shell code.
    std::wstring cmd=L"dotnet.exe build \""+project.wstring()+L"\" --configuration Release --output \""+output.wstring()+L"\" -p:RuntimeProject=\""+(ModuleDir()/"SDK/Tartarus.Runtime.csproj").wstring()+L"\" --configfile \""+(ModuleDir()/"SDK/NuGet.Config").wstring()+L"\" --nologo";
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    auto log=std::filesystem::u8path(ProjectPaths::Resolve("Scripts/build.log"));
    HANDLE file=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return Fail("Cannot write Scripts/build.log");
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(input==INVALID_HANDLE_VALUE) { CloseHandle(file); return Fail("Cannot open build input"); }
    STARTUPINFOW startup{}; startup.cb=sizeof startup; startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=file; startup.hStdError=file; startup.hStdInput=input;
    PROCESS_INFORMATION process{};
    const BOOL ok=CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,project.parent_path().c_str(),&startup,&process);
    CloseHandle(input); CloseHandle(file); if(!ok) return Fail("Cannot start dotnet build; install the .NET 10 SDK");
    CloseHandle(process.hThread); buildProcess=process.hProcess; Log::Info("Building C# gameplay (Scripts/build.log)..."); return true;
}
void Poll() {
    if(buildProcess && WaitForSingleObject(buildProcess,0)==WAIT_OBJECT_0) {
        DWORD code=1; GetExitCodeProcess(buildProcess,&code); CloseHandle(buildProcess); buildProcess=nullptr;
        if(code) {
            std::ifstream log(ProjectPaths::Resolve(editorBuild?"Scripts/editor-build.log":"Scripts/build.log")); std::string content((std::istreambuf_iterator<char>(log)),{});
            Fail("Build failed; running assembly retained.\n"+content); return;
        }
        Log::Info("C# build succeeded.");
        descriptionCache.clear();
        if(!dispatch) { attemptedLoad=false; EnsureLoaded(); }
        if(!editorBuild && editorBuildNext) {
            editorBuildNext=false;
            const auto project=std::filesystem::u8path(ProjectPaths::Resolve("Scripts/Tartarus.Editor.csproj"));
            std::wstring cmd=L"dotnet.exe build \""+project.wstring()+L"\" --configuration Release --output \""+(project.parent_path()/"bin").wstring()+L"\" -p:RuntimeProject=\""+(ModuleDir()/"SDK/Tartarus.Runtime.csproj").wstring()+L"\" --configfile \""+(ModuleDir()/"SDK/NuGet.Config").wstring()+L"\" --nologo";
            SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
            HANDLE file=CreateFileW((project.parent_path()/"editor-build.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            STARTUPINFOW startup{};startup.cb=sizeof startup;startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=file;startup.hStdError=file;startup.hStdInput=input;
            PROCESS_INFORMATION process{};
            const bool ok=file!=INVALID_HANDLE_VALUE && input!=INVALID_HANDLE_VALUE && CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,project.parent_path().c_str(),&startup,&process);
            if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
            if(input!=INVALID_HANDLE_VALUE) CloseHandle(input);
            if(!ok) {Fail("Cannot start C# editor build");return;}
            CloseHandle(process.hThread);buildProcess=process.hProcess;editorBuild=true;Log::Info("Building C# editor tools (Scripts/editor-build.log)...");return;
        }
    }
    if(buildPending && !buildProcess && std::chrono::steady_clock::now()>=buildAfter) { Build(); return; }
    if(!dispatch || buildProcess) return;
    std::error_code ec; auto now=std::filesystem::last_write_time(GameplayPath(),ec);
    if(!ec && now!=assemblyTime) {
        const auto path=GameplayPath().u8string();
        if(dispatch(0,const_cast<char*>(path.c_str()),kVersion,reinterpret_cast<void*>(&Native))==0) { error.clear(); rejectedScripts.clear(); descriptionCache.clear(); }
        else Fail("Reload rejected; previous C# assembly retained.");
        assemblyTime=now;
    }
}
void DrawEditorScripts(World& world,AssetLibrary& assets,float dt,const std::function<int(int,NativeRequest&)>& services) {
    World* previousWorld=activeWorld;AssetLibrary* previousAssets=activeAssets;
    activeWorld=&world;activeAssets=&assets;editorServices=&services;
    Poll();
    auto project=std::filesystem::u8path(ProjectPaths::Resolve("Scripts/bin/Tartarus.Editor.dll"));
    const auto staged=ModuleDir()/"Tartarus.Editor.dll";
    std::error_code ec;
    if(!std::filesystem::exists(project,ec) || (std::filesystem::exists(staged,ec) && std::filesystem::last_write_time(staged,ec)>std::filesystem::last_write_time(project,ec))) project=staged;
    if(std::filesystem::exists(project,ec) && !buildProcess) {
        const auto stamp=std::filesystem::last_write_time(project,ec);
        if(!ec && (!editorAttempted || stamp!=editorAssemblyTime)) {
            editorAttempted=true;editorAssemblyTime=stamp;const auto path=project.u8string();
            if(!Invoke(8,const_cast<char*>(path.c_str()),kVersion)) Log::Error("C# editor reload rejected; previous tools retained.");
        }
    }
    if(editorAttempted) {NativeRequest frame;frame.Value=dt;Invoke(9,&frame,sizeof frame);}
    editorServices=nullptr;activeWorld=previousWorld;activeAssets=previousAssets;
}
void StopEditorScripts() {if(dispatch) Invoke(10,nullptr,0);StopScriptSounds();editorAttempted=false;}
bool EditorToolsVisible() {NativeRequest frame;return dispatch && Invoke(12,&frame,sizeof frame) && frame.Result!=0;}
void SetEditorToolsVisible(bool visible) {if(dispatch){NativeRequest frame;frame.Entity=1;frame.Result=visible?1:0;Invoke(12,&frame,sizeof frame);}}
bool DrawEditorInspector(World& world,AssetLibrary& assets,NativeRequest& request,
    const std::function<int(int,NativeRequest&)>& services) {
    if(!editorAttempted) return false;
    auto* previousWorld=activeWorld;auto* previousAssets=activeAssets;
    activeWorld=&world;activeAssets=&assets;editorServices=&services;
    const bool ok=Invoke(11,&request,sizeof request);
    editorServices=nullptr;activeWorld=previousWorld;activeAssets=previousAssets;
    return ok && request.Result!=0;
}
}
