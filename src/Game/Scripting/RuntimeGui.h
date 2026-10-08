#pragma once
#include "ScriptAbi.h"
namespace Scripting {
class ScopedRuntimeGui {
public:
    ScopedRuntimeGui();~ScopedRuntimeGui();
    ScopedRuntimeGui(const ScopedRuntimeGui&)=delete;ScopedRuntimeGui& operator=(const ScopedRuntimeGui&)=delete;
private:std::uint32_t m_PreviousToken;int m_PreviousDepth;
};
int RuntimeGuiService(NativeRequest& request);
}
