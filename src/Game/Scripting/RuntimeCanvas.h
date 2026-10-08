#pragma once
#include "ScriptAbi.h"
class HudText;
namespace Scripting {
class ScopedRuntimeCanvas {
public:
    explicit ScopedRuntimeCanvas(HudText& canvas);
    ~ScopedRuntimeCanvas();
    ScopedRuntimeCanvas(const ScopedRuntimeCanvas&)=delete;
    ScopedRuntimeCanvas& operator=(const ScopedRuntimeCanvas&)=delete;
private:
    HudText* m_Previous;
    std::uint32_t m_PreviousToken;
};
int RuntimeCanvasService(NativeRequest& request);
}
