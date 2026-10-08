#pragma once
#include "ScriptAbi.h"
#include <string>
#include <functional>
class World;
class AssetLibrary;
namespace Scripting {
bool EnsureLoaded();
std::uint64_t CodeGeneration();
bool Invoke(int operation, void* frame, int size);
bool InvokeProjectWithTrace(World& world,const char* operation,void* data,int size, const std::function<void(const NativeRequest&)>& trace);
const std::string& LastError();
void Tick(World& world, AssetLibrary& assets, float dt, bool fixed = false);
void Stop(World* world = nullptr, AssetLibrary* assets = nullptr);
bool Build();
void Poll();
bool Building();
void RequestBuild();
bool BuildPending();
std::string Describe(const std::string& className = "");
bool ResolveScriptFields(const std::string& className,const std::string& fields,std::string& resolved);
bool InvokeProject(const char* operation,void* data,int size);
bool RequestProject(const std::string& operation,const std::string& data,std::string& result);
void DrawEditorScripts(World& world, AssetLibrary& assets, float dt,
    const std::function<int(int,NativeRequest&)>& editorServices);
void StopEditorScripts();
bool EditorToolsVisible();
void SetEditorToolsVisible(bool visible);
bool DrawEditorInspector(World& world,AssetLibrary& assets,NativeRequest& request,
    const std::function<int(int,NativeRequest&)>& editorServices);
}
