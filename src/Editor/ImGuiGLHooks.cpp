// Dear ImGui's OpenGL backend backs up ~20 GL states with glGet* / glIsEnabled around every render
// and restores them after. On a threaded driver (NVIDIA's) each read makes the engine thread wait
// for the driver's worker to drain the whole frame, ~9% of a Sandbox frame. The build compiles a
// copy of the backend whose reads call these instead (CMakeLists.txt, imgui_patched).
//
// State the loader shadows (viewport, caps, blend funcs - gl.h) is answered truthfully from it.
// The rest the engine never relies on across the ImGui render - bound program / texture / VAO /
// array buffer / sampler, active texture unit, polygon mode, blend equation, scissor box, clip
// origin - is answered with a canonical value, so ImGui "restores" to that. The engine's own
// binding cache is invalidated right after EndFrame (main.cpp) and at BeginFrame
// (EditorLayer.cpp), so nothing assumes the pre-ImGui bindings survived.
#include "gl.h"

extern "C" void TartarusImGui_GetIntegerv(GLenum pname, GLint* data) {
    switch (pname) {
    case 0x84E0: data[0] = 0x84C0; return;                 // GL_ACTIVE_TEXTURE -> GL_TEXTURE0
    case 0x8B8D:                                           // GL_CURRENT_PROGRAM
    case 0x8069:                                           // GL_TEXTURE_BINDING_2D
    case 0x8919:                                           // GL_SAMPLER_BINDING
    case 0x8894:                                           // GL_ARRAY_BUFFER_BINDING
    case 0x85B5: data[0] = 0; return;                      // GL_VERTEX_ARRAY_BINDING
    case 0x0B40: data[0] = data[1] = 0x1B02; return;        // GL_POLYGON_MODE -> GL_FILL
    case 0x8009:                                           // GL_BLEND_EQUATION_RGB
    case 0x883D: data[0] = 0x8006; return;                 // GL_BLEND_EQUATION_ALPHA -> GL_FUNC_ADD
    case 0x935C: data[0] = 0x8CA1; return;                 // GL_CLIP_ORIGIN -> GL_LOWER_LEFT
    case 0x0C10: glGetIntegerv(0x0BA2, data); return;      // GL_SCISSOR_BOX: nothing else scissors
    default: glGetIntegerv(pname, data); return;           // the shadow, or the driver
    }
}

extern "C" GLboolean TartarusImGui_IsEnabled(GLenum cap) { return glIsEnabled(cap); }
