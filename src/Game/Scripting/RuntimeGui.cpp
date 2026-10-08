#include "RuntimeGui.h"
#include "imgui.h"
#include <cmath>
namespace Scripting {
namespace {thread_local std::uint32_t token=0,next=0;thread_local int depth=0;}
ScopedRuntimeGui::ScopedRuntimeGui():m_PreviousToken(token),m_PreviousDepth(depth){if(++next==0)++next;token=ImGui::GetCurrentContext()?next:0;depth=0;}
ScopedRuntimeGui::~ScopedRuntimeGui(){while(depth>0){ImGui::End();--depth;}token=m_PreviousToken;depth=m_PreviousDepth;}
int RuntimeGuiService(NativeRequest& r){
    if(!token || !ImGui::GetCurrentContext())return 0;
    if(r.Script==0){r.Entity=token;return 1;}
    if(r.Entity!=token || !std::isfinite(r.Value) || !std::isfinite(r.A.x) || !std::isfinite(r.A.y) || !std::isfinite(r.A.z))return 0;
    const char* text=r.Text?r.Text:"";
    switch(r.Script){
    case 1:{bool open=r.Value!=0;ImGui::SetNextWindowPos({r.A.y,r.A.z},ImGuiCond_FirstUseEver);ImGui::SetNextWindowSize({r.A.x,0},ImGuiCond_FirstUseEver);ImGui::SetNextWindowBgAlpha(.92f);r.C.x=ImGui::Begin(text,&open,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_AlwaysAutoResize)?1.0f:0.0f;++depth;r.Value=open?1.0f:0.0f;return 1;}
    case 2:if(depth<=0)return 0;ImGui::End();--depth;return 1;
    case 3:if(depth<=0)return 0;ImGui::SeparatorText(text);return 1;
    case 4:{if(depth<=0)return 0;bool value=r.Value!=0;ImGui::Checkbox(text,&value);r.Value=value?1.0f:0.0f;return 1;}
    case 5:if(depth<=0 || r.A.y<r.A.x)return 0;ImGui::SliderFloat(text,&r.Value,r.A.x,r.A.y,"%.2f");return 1;
    case 6:if(depth<=0)return 0;r.Value=ImGui::Button(text)?1.0f:0.0f;return 1;
    case 7:if(depth<=0)return 0;ImGui::SameLine();return 1;
    case 8:if(depth<=0)return 0;if(r.Value!=0)ImGui::BeginDisabled();ImGui::TextUnformatted(text);if(r.Value!=0)ImGui::EndDisabled();return 1;
    default:return 0;
    }
}
}
