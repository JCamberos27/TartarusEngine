#pragma once
#include "ScriptAbi.h"
class World;
class AssetLibrary;
namespace Scripting {
// UTF-8 replies live until the next service call on this thread; managed callers copy immediately.
int ScriptServices(World& world, AssetLibrary* assets, int operation, NativeRequest& request);
int PhysicsServices(int operation, NativeRequest& request);
void StopScriptSounds();
}
