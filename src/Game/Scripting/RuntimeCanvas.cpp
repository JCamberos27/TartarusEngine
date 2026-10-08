#include "RuntimeCanvas.h"
#include "HudText.h"
#include <cmath>
namespace Scripting {
namespace {thread_local HudText* canvas=nullptr;thread_local std::uint32_t token=0,next=0;}
ScopedRuntimeCanvas::ScopedRuntimeCanvas(HudText& value):m_Previous(canvas),m_PreviousToken(token) {
    canvas=&value;if(++next==0)++next;token=next;
}
ScopedRuntimeCanvas::~ScopedRuntimeCanvas(){canvas=m_Previous;token=m_PreviousToken;}
int RuntimeCanvasService(NativeRequest& r) {
    if(!canvas)return 0;
    if(r.Script==0){r.Entity=token;r.A={(float)canvas->Width(),(float)canvas->Height(),0};r.Value=canvas->Scale();return 1;}
    if(r.Entity!=token || !std::isfinite(r.Value) || !std::isfinite(r.A.x) || !std::isfinite(r.A.y) || !std::isfinite(r.A.z) || !std::isfinite(r.B.x) || !std::isfinite(r.B.y) || !std::isfinite(r.B.z) || !std::isfinite(r.C.x) || !std::isfinite(r.C.y))return 0;
    const glm::vec4 color(r.B.x,r.B.y,r.B.z,r.Value);
    switch(r.Script) {
    case 1:r.Value=canvas->Measure(r.Text?r.Text:"",r.Value);return 1;
    case 2:r.Value=canvas->LineHeight(r.Value);return 1;
    case 3:if(r.C.x<0 || r.C.x>2)return 0;r.Value=canvas->Text(r.A.x,r.A.y,r.Text?r.Text:"",r.A.z,color,(HudText::Align)(int)r.C.x,r.C.y!=0);return 1;
    case 4:canvas->Rect(r.A.x,r.A.y,r.A.z,r.C.x,color);return 1;
    case 5:canvas->Line({r.A.x,r.A.y},{r.C.x,r.C.y},r.A.z,color);return 1;
    default:return 0;
    }
}
}
