#pragma once
#include "ScriptAbi.h"
#include <string>
#include <functional>
class World;
class AssetLibrary;
namespace Scripting {
bool EnsureLoaded();
bool Invoke(int operation, void* frame, int size);
bool InvokeShot(World& world, ShotFrame& frame, const std::function<void(const NativeRequest&)>& trace);
const std::string& LastError();
void Tick(World& world, AssetLibrary& assets, float dt, bool fixed = false);
void Stop(World* world = nullptr, AssetLibrary* assets = nullptr);
bool Build();
void Poll();
bool Building();
void RequestBuild();
bool BuildPending();
std::string Describe(const std::string& className = "");
void DrawEditorScripts(World& world, AssetLibrary& assets, float dt,
    const std::function<int(int,NativeRequest&)>& editorServices);
void StopEditorScripts();
bool EditorToolsVisible();
void SetEditorToolsVisible(bool visible);
bool DrawEditorInspector(World& world,AssetLibrary& assets,NativeRequest& request,
    const std::function<int(int,NativeRequest&)>& editorServices);
}
