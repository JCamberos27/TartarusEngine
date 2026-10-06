#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct CSharpScriptComponent;
namespace Scripting {
struct ScriptSlot {
    std::uint32_t Id=0;
    std::string Source, Class, Fields="{}";
    bool Enabled=true;
};
std::vector<ScriptSlot> GetSlots(const CSharpScriptComponent& component);
void SetSlots(CSharpScriptComponent& component,const std::vector<ScriptSlot>& slots);
void Attach(CSharpScriptComponent& component,const std::string& source,const std::string& className);
}
