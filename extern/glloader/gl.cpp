// The loader defines and calls the real entry points; the shadow is at the bottom.
#define TARTARUS_GL_NO_STATE_SHADOW
#include "gl.h"
#include <windows.h>

PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = nullptr;
PFNGLBINDVERTEXARRAYPROC glBindVertexArray = nullptr;
PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays = nullptr;
PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
PFNGLBUFFERDATAPROC glBufferData = nullptr;
PFNGLBUFFERSUBDATAPROC glBufferSubData = nullptr;
PFNGLDELETEBUFFERSPROC glDeleteBuffers = nullptr;
PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer = nullptr;
PFNGLVERTEXATTRIBIPOINTERPROC glVertexAttribIPointer = nullptr;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = nullptr;
PFNGLCREATESHADERPROC glCreateShader = nullptr;
PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
PFNGLDELETESHADERPROC glDeleteShader = nullptr;
PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
PFNGLATTACHSHADERPROC glAttachShader = nullptr;
PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
PFNGLDELETEPROGRAMPROC glDeleteProgram = nullptr;
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv = nullptr;
PFNGLUNIFORM2FPROC glUniform2f = nullptr;
PFNGLUNIFORM1IPROC glUniform1i = nullptr;
PFNGLUNIFORM1FPROC glUniform1f = nullptr;
PFNGLUNIFORM3FPROC glUniform3f = nullptr;
PFNGLUNIFORM3FVPROC glUniform3fv = nullptr;
PFNGLUNIFORM1FVPROC glUniform1fv = nullptr;
PFNGLUNIFORM4FPROC glUniform4f = nullptr;
PFNGLACTIVETEXTUREPROC glActiveTexture = nullptr;
PFNGLGENERATEMIPMAPPROC glGenerateMipmap = nullptr;
PFNGLDRAWELEMENTSPROC glDrawElements = nullptr;
PFNGLDRAWELEMENTSBASEVERTEXPROC glDrawElementsBaseVertex = nullptr;
PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation = nullptr;
PFNGLDETACHSHADERPROC glDetachShader = nullptr;
PFNGLBINDSAMPLERPROC glBindSampler = nullptr;
PFNGLBLENDEQUATIONPROC glBlendEquation = nullptr;
PFNGLBLENDEQUATIONSEPARATEPROC glBlendEquationSeparate = nullptr;
PFNGLBLENDFUNCSEPARATEPROC glBlendFuncSeparate = nullptr;
PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = nullptr;
PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers = nullptr;
PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers = nullptr;
PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer = nullptr;
PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage = nullptr;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer = nullptr;
PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers = nullptr;
PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer = nullptr;

// ---- Lighting / HDR overhaul: extended entry points -----------------------------------
PFNGLGETSTRINGIPROC glGetStringi = nullptr;
PFNGLBINDBUFFERBASEPROC glBindBufferBase = nullptr;
PFNGLBINDBUFFERRANGEPROC glBindBufferRange = nullptr;
PFNGLDRAWBUFFERSPROC glDrawBuffers = nullptr;
PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC glRenderbufferStorageMultisample = nullptr;
PFNGLTEXIMAGE2DMULTISAMPLEPROC glTexImage2DMultisample = nullptr;
PFNGLCREATEBUFFERSPROC glCreateBuffers = nullptr;
PFNGLNAMEDBUFFERSTORAGEPROC glNamedBufferStorage = nullptr;
PFNGLNAMEDBUFFERSUBDATAPROC glNamedBufferSubData = nullptr;
PFNGLNAMEDBUFFERDATAPROC glNamedBufferData = nullptr;
PFNGLVERTEXARRAYBINDINGDIVISORPROC glVertexArrayBindingDivisor = nullptr;
PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC glDrawArraysInstancedBaseInstance = nullptr;
PFNGLDRAWELEMENTSINSTANCEDPROC glDrawElementsInstanced = nullptr;
PFNGLGETNAMEDBUFFERSUBDATAPROC glGetNamedBufferSubData = nullptr;
PFNGLCREATETEXTURESPROC glCreateTextures = nullptr;
PFNGLTEXTURESTORAGE2DPROC glTextureStorage2D = nullptr;
PFNGLTEXTURESTORAGE3DPROC glTextureStorage3D = nullptr;
PFNGLTEXTURESTORAGE2DMULTISAMPLEPROC glTextureStorage2DMultisample = nullptr;
PFNGLTEXTURESUBIMAGE2DPROC glTextureSubImage2D = nullptr;
PFNGLCOMPRESSEDTEXTURESUBIMAGE2DPROC glCompressedTextureSubImage2D = nullptr;
PFNGLTEXTURESUBIMAGE3DPROC glTextureSubImage3D = nullptr;
PFNGLTEXTUREPARAMETERIPROC glTextureParameteri = nullptr;
PFNGLTEXTUREPARAMETERFVPROC glTextureParameterfv = nullptr;
PFNGLGENERATETEXTUREMIPMAPPROC glGenerateTextureMipmap = nullptr;
PFNGLBINDTEXTUREUNITPROC glBindTextureUnit = nullptr;
PFNGLCREATEFRAMEBUFFERSPROC glCreateFramebuffers = nullptr;
PFNGLNAMEDFRAMEBUFFERTEXTUREPROC glNamedFramebufferTexture = nullptr;
PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC glNamedFramebufferTextureLayer = nullptr;
PFNGLNAMEDFRAMEBUFFERRENDERBUFFERPROC glNamedFramebufferRenderbuffer = nullptr;
PFNGLNAMEDFRAMEBUFFERDRAWBUFFERSPROC glNamedFramebufferDrawBuffers = nullptr;
PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC glCheckNamedFramebufferStatus = nullptr;
PFNGLBLITNAMEDFRAMEBUFFERPROC glBlitNamedFramebuffer = nullptr;
PFNGLCLEARNAMEDFRAMEBUFFERFVPROC glClearNamedFramebufferfv = nullptr;
PFNGLCREATEVERTEXARRAYSPROC glCreateVertexArrays = nullptr;
PFNGLENABLEVERTEXARRAYATTRIBPROC glEnableVertexArrayAttrib = nullptr;
PFNGLVERTEXARRAYATTRIBFORMATPROC glVertexArrayAttribFormat = nullptr;
PFNGLVERTEXARRAYATTRIBIFORMATPROC glVertexArrayAttribIFormat = nullptr;
PFNGLVERTEXARRAYATTRIBBINDINGPROC glVertexArrayAttribBinding = nullptr;
PFNGLVERTEXARRAYVERTEXBUFFERPROC glVertexArrayVertexBuffer = nullptr;
PFNGLVERTEXARRAYELEMENTBUFFERPROC glVertexArrayElementBuffer = nullptr;
PFNGLDISPATCHCOMPUTEPROC glDispatchCompute = nullptr;
PFNGLMEMORYBARRIERPROC glMemoryBarrier = nullptr;
PFNGLBINDIMAGETEXTUREPROC glBindImageTexture = nullptr;
PFNGLUNIFORM2IPROC glUniform2i = nullptr;

// GPU timer queries — Profiler's GPU-side timing (#197).
PFNGLGENQUERIESPROC glGenQueries = nullptr;
PFNGLDELETEQUERIESPROC glDeleteQueries = nullptr;
PFNGLBEGINQUERYPROC glBeginQuery = nullptr;
PFNGLENDQUERYPROC glEndQuery = nullptr;
PFNGLGETQUERYOBJECTIVPROC glGetQueryObjectiv = nullptr;
PFNGLGETQUERYOBJECTUI64VPROC glGetQueryObjectui64v = nullptr;
PFNGLQUERYCOUNTERPROC glQueryCounter = nullptr;

// Async pixel readback (PBO) — adaptive HUD contrast sampling without a GPU stall (#178).
PFNGLMAPNAMEDBUFFERPROC glMapNamedBuffer = nullptr;
PFNGLUNMAPNAMEDBUFFERPROC glUnmapNamedBuffer = nullptr;

// Fence sync + buffer-to-buffer copy — deferred cluster-overflow readback (PERF-203).
PFNGLCOPYNAMEDBUFFERSUBDATAPROC glCopyNamedBufferSubData = nullptr;
PFNGLFENCESYNCPROC glFenceSync = nullptr;
PFNGLCLIENTWAITSYNCPROC glClientWaitSync = nullptr;
PFNGLDELETESYNCPROC glDeleteSync = nullptr;

namespace {
void* LoadGLFunc(const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 || p == (void*)0x3 || p == (void*)-1) {
        // GL 1.1 entry points live in opengl32.dll itself, which a live WGL context has already
        // loaded. Look it up once instead of LoadLibraryA per symbol, which leaked a module
        // reference on every call (#157).
        static const HMODULE opengl32 = GetModuleHandleW(L"opengl32.dll");
        p = opengl32 ? (void*)GetProcAddress(opengl32, name) : nullptr;
    }
    return p;
}
}

bool GLLoader_Init() {
    bool ok = true;
#define LOAD(type, var) var = (type)LoadGLFunc(#var); ok = ok && (var != nullptr);
    LOAD(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)
    LOAD(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)
    LOAD(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)
    LOAD(PFNGLGENBUFFERSPROC, glGenBuffers)
    LOAD(PFNGLBINDBUFFERPROC, glBindBuffer)
    LOAD(PFNGLBUFFERDATAPROC, glBufferData)
    LOAD(PFNGLBUFFERSUBDATAPROC, glBufferSubData)
    LOAD(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)
    LOAD(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)
    LOAD(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer)
    LOAD(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)
    LOAD(PFNGLCREATESHADERPROC, glCreateShader)
    LOAD(PFNGLSHADERSOURCEPROC, glShaderSource)
    LOAD(PFNGLCOMPILESHADERPROC, glCompileShader)
    LOAD(PFNGLGETSHADERIVPROC, glGetShaderiv)
    LOAD(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)
    LOAD(PFNGLDELETESHADERPROC, glDeleteShader)
    LOAD(PFNGLCREATEPROGRAMPROC, glCreateProgram)
    LOAD(PFNGLATTACHSHADERPROC, glAttachShader)
    LOAD(PFNGLLINKPROGRAMPROC, glLinkProgram)
    LOAD(PFNGLGETPROGRAMIVPROC, glGetProgramiv)
    LOAD(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)
    LOAD(PFNGLUSEPROGRAMPROC, glUseProgram)
    LOAD(PFNGLDELETEPROGRAMPROC, glDeleteProgram)
    LOAD(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)
    LOAD(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)
    LOAD(PFNGLUNIFORM2FPROC, glUniform2f)
    LOAD(PFNGLUNIFORM1IPROC, glUniform1i)
    LOAD(PFNGLUNIFORM1FPROC, glUniform1f)
    LOAD(PFNGLUNIFORM3FPROC, glUniform3f)
    LOAD(PFNGLUNIFORM3FVPROC, glUniform3fv)
    LOAD(PFNGLUNIFORM1FVPROC, glUniform1fv)
    LOAD(PFNGLUNIFORM4FPROC, glUniform4f)
    LOAD(PFNGLACTIVETEXTUREPROC, glActiveTexture)
    LOAD(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)
    LOAD(PFNGLDRAWELEMENTSPROC, glDrawElements)
    LOAD(PFNGLDRAWELEMENTSBASEVERTEXPROC, glDrawElementsBaseVertex)
    LOAD(PFNGLGETATTRIBLOCATIONPROC, glGetAttribLocation)
    LOAD(PFNGLDETACHSHADERPROC, glDetachShader)
    LOAD(PFNGLBINDSAMPLERPROC, glBindSampler)
    LOAD(PFNGLBLENDEQUATIONPROC, glBlendEquation)
    LOAD(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)
    LOAD(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)
    LOAD(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)
    LOAD(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)
    LOAD(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)
    LOAD(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)
    LOAD(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)
    LOAD(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)
    LOAD(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)
    LOAD(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)
    LOAD(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)
    LOAD(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers)
    LOAD(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)

    // ---- Lighting / HDR overhaul: extended entry points -------------------------------
    LOAD(PFNGLGETSTRINGIPROC, glGetStringi)
    LOAD(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)
    LOAD(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange)
    LOAD(PFNGLDRAWBUFFERSPROC, glDrawBuffers)
    LOAD(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample)
    LOAD(PFNGLTEXIMAGE2DMULTISAMPLEPROC, glTexImage2DMultisample)
    LOAD(PFNGLCREATEBUFFERSPROC, glCreateBuffers)
    LOAD(PFNGLNAMEDBUFFERSTORAGEPROC, glNamedBufferStorage)
    LOAD(PFNGLNAMEDBUFFERSUBDATAPROC, glNamedBufferSubData)
    LOAD(PFNGLNAMEDBUFFERDATAPROC, glNamedBufferData)
    LOAD(PFNGLVERTEXARRAYBINDINGDIVISORPROC, glVertexArrayBindingDivisor)
    LOAD(PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC, glDrawArraysInstancedBaseInstance)
    LOAD(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced)
    LOAD(PFNGLGETNAMEDBUFFERSUBDATAPROC, glGetNamedBufferSubData)
    LOAD(PFNGLCREATETEXTURESPROC, glCreateTextures)
    LOAD(PFNGLTEXTURESTORAGE2DPROC, glTextureStorage2D)
    LOAD(PFNGLTEXTURESTORAGE3DPROC, glTextureStorage3D)
    LOAD(PFNGLTEXTURESTORAGE2DMULTISAMPLEPROC, glTextureStorage2DMultisample)
    LOAD(PFNGLTEXTURESUBIMAGE2DPROC, glTextureSubImage2D)
    LOAD(PFNGLCOMPRESSEDTEXTURESUBIMAGE2DPROC, glCompressedTextureSubImage2D)
    LOAD(PFNGLTEXTURESUBIMAGE3DPROC, glTextureSubImage3D)
    LOAD(PFNGLTEXTUREPARAMETERIPROC, glTextureParameteri)
    LOAD(PFNGLTEXTUREPARAMETERFVPROC, glTextureParameterfv)
    LOAD(PFNGLGENERATETEXTUREMIPMAPPROC, glGenerateTextureMipmap)
    LOAD(PFNGLBINDTEXTUREUNITPROC, glBindTextureUnit)
    LOAD(PFNGLCREATEFRAMEBUFFERSPROC, glCreateFramebuffers)
    LOAD(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, glNamedFramebufferTexture)
    LOAD(PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC, glNamedFramebufferTextureLayer)
    LOAD(PFNGLNAMEDFRAMEBUFFERRENDERBUFFERPROC, glNamedFramebufferRenderbuffer)
    LOAD(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERSPROC, glNamedFramebufferDrawBuffers)
    LOAD(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC, glCheckNamedFramebufferStatus)
    LOAD(PFNGLBLITNAMEDFRAMEBUFFERPROC, glBlitNamedFramebuffer)
    LOAD(PFNGLCLEARNAMEDFRAMEBUFFERFVPROC, glClearNamedFramebufferfv)
    LOAD(PFNGLCREATEVERTEXARRAYSPROC, glCreateVertexArrays)
    LOAD(PFNGLENABLEVERTEXARRAYATTRIBPROC, glEnableVertexArrayAttrib)
    LOAD(PFNGLVERTEXARRAYATTRIBFORMATPROC, glVertexArrayAttribFormat)
    LOAD(PFNGLVERTEXARRAYATTRIBIFORMATPROC, glVertexArrayAttribIFormat)
    LOAD(PFNGLVERTEXARRAYATTRIBBINDINGPROC, glVertexArrayAttribBinding)
    LOAD(PFNGLVERTEXARRAYVERTEXBUFFERPROC, glVertexArrayVertexBuffer)
    LOAD(PFNGLVERTEXARRAYELEMENTBUFFERPROC, glVertexArrayElementBuffer)
    LOAD(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute)
    LOAD(PFNGLMEMORYBARRIERPROC, glMemoryBarrier)
    LOAD(PFNGLBINDIMAGETEXTUREPROC, glBindImageTexture)
    LOAD(PFNGLUNIFORM2IPROC, glUniform2i)

    // GPU timer queries — Profiler's GPU-side timing (#197).
    LOAD(PFNGLGENQUERIESPROC, glGenQueries)
    LOAD(PFNGLDELETEQUERIESPROC, glDeleteQueries)
    LOAD(PFNGLBEGINQUERYPROC, glBeginQuery)
    LOAD(PFNGLENDQUERYPROC, glEndQuery)
    LOAD(PFNGLGETQUERYOBJECTIVPROC, glGetQueryObjectiv)
    LOAD(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v)
    LOAD(PFNGLQUERYCOUNTERPROC, glQueryCounter)

    // Async pixel readback (PBO) — adaptive HUD contrast sampling without a GPU stall (#178).
    LOAD(PFNGLMAPNAMEDBUFFERPROC, glMapNamedBuffer)
    LOAD(PFNGLUNMAPNAMEDBUFFERPROC, glUnmapNamedBuffer)

    // Fence sync + buffer-to-buffer copy — deferred cluster-overflow readback (PERF-203).
    LOAD(PFNGLCOPYNAMEDBUFFERSUBDATAPROC, glCopyNamedBufferSubData)
    LOAD(PFNGLFENCESYNCPROC, glFenceSync)
    LOAD(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync)
    LOAD(PFNGLDELETESYNCPROC, glDeleteSync)
#undef LOAD
    return ok;
}

// --- Render-state shadow (gl.h) ---------------------------------------------------------------
namespace {
struct Tracked {
    bool Valid = false;
    GLint V[4] = {};
};
struct Cap {
    GLenum Id;
    bool Valid;
    bool On;
};
// The capabilities the engine toggles; any other passes straight through untracked.
Cap g_Caps[] = {
    {0x0B71, false, false}, // GL_DEPTH_TEST
    {0x0B44, false, false}, // GL_CULL_FACE
    {0x0BE2, false, false}, // GL_BLEND
    {0x0C11, false, false}, // GL_SCISSOR_TEST
    {0x8037, false, false}, // GL_POLYGON_OFFSET_FILL
    {0x864F, false, false}, // GL_DEPTH_CLAMP
    {0x0B90, false, false}, // GL_STENCIL_TEST
    {0x8DB9, false, false}, // GL_FRAMEBUFFER_SRGB
    {0x809D, false, false}, // GL_MULTISAMPLE
    {0x809E, false, false}, // GL_SAMPLE_ALPHA_TO_COVERAGE
    {0x884F, false, false}, // GL_TEXTURE_CUBE_MAP_SEAMLESS
    {0x8F9D, false, false}, // GL_PRIMITIVE_RESTART
};
Tracked g_Viewport, g_DepthFunc, g_DepthMask, g_CullMode, g_Blend, g_DrawFbo, g_ReadFbo;

Cap* FindCap(GLenum id) {
    for (Cap& c : g_Caps)
        if (c.Id == id) return &c;
    return nullptr;
}
bool CapOn(Cap& c) {
    if (!c.Valid) {
        c.On = ::glIsEnabled(c.Id) == GL_TRUE;
        c.Valid = true;
    }
    return c.On;
}
// The tracked value `pname` reads, filled from the driver the first time after an invalidate.
const GLint* Read(Tracked& t, GLenum pname) {
    if (!t.Valid) {
        ::glGetIntegerv(pname, t.V);
        t.Valid = true;
    }
    return t.V;
}
const GLint* ReadBlend() {
    if (!g_Blend.Valid) {
        ::glGetIntegerv(0x80C9, &g_Blend.V[0]); // GL_BLEND_SRC_RGB
        ::glGetIntegerv(0x80C8, &g_Blend.V[1]); // GL_BLEND_DST_RGB
        ::glGetIntegerv(0x80CB, &g_Blend.V[2]); // GL_BLEND_SRC_ALPHA
        ::glGetIntegerv(0x80CA, &g_Blend.V[3]); // GL_BLEND_DST_ALPHA
        g_Blend.Valid = true;
    }
    return g_Blend.V;
}
// Records a single-value state; false when it already held `v` (the GL call can be skipped).
bool Set1(Tracked& t, GLint v) {
    if (t.Valid && t.V[0] == v) return false;
    t.V[0] = v;
    t.Valid = true;
    return true;
}
} // namespace

void GLStateShadow_Invalidate() {
    for (Cap& c : g_Caps) c.Valid = false;
    Tracked* const all[] = {&g_Viewport, &g_DepthFunc, &g_DepthMask, &g_CullMode, &g_Blend, &g_DrawFbo, &g_ReadFbo};
    for (Tracked* t : all) t->Valid = false;
}

namespace glshadow {

void __stdcall Viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    GLint* v = g_Viewport.V;
    if (g_Viewport.Valid && v[0] == x && v[1] == y && v[2] == w && v[3] == h) return;
    ::glViewport(x, y, w, h);
    v[0] = x; v[1] = y; v[2] = w; v[3] = h;
    g_Viewport.Valid = true;
}

void __stdcall Enable(GLenum cap) {
    Cap* c = FindCap(cap);
    if (c && c->Valid && c->On) return;
    ::glEnable(cap);
    if (c) { c->Valid = true; c->On = true; }
}

void __stdcall Disable(GLenum cap) {
    Cap* c = FindCap(cap);
    if (c && c->Valid && !c->On) return;
    ::glDisable(cap);
    if (c) { c->Valid = true; c->On = false; }
}

GLboolean __stdcall IsEnabled(GLenum cap) {
    Cap* c = FindCap(cap);
    if (!c) return ::glIsEnabled(cap);
    return CapOn(*c) ? GL_TRUE : GL_FALSE;
}

void __stdcall DepthFunc(GLenum func) {
    if (Set1(g_DepthFunc, (GLint)func)) ::glDepthFunc(func);
}

void __stdcall DepthMask(GLboolean flag) {
    if (Set1(g_DepthMask, flag ? 1 : 0)) ::glDepthMask(flag);
}

void __stdcall CullFace(GLenum mode) {
    if (Set1(g_CullMode, (GLint)mode)) ::glCullFace(mode);
}

void __stdcall BlendFuncSeparate(GLenum srcRgb, GLenum dstRgb, GLenum srcA, GLenum dstA) {
    GLint* v = g_Blend.V;
    if (g_Blend.Valid && v[0] == (GLint)srcRgb && v[1] == (GLint)dstRgb && v[2] == (GLint)srcA && v[3] == (GLint)dstA)
        return;
    ::glBlendFuncSeparate(srcRgb, dstRgb, srcA, dstA);
    v[0] = (GLint)srcRgb; v[1] = (GLint)dstRgb; v[2] = (GLint)srcA; v[3] = (GLint)dstA;
    g_Blend.Valid = true;
}

void __stdcall BlendFunc(GLenum src, GLenum dst) { BlendFuncSeparate(src, dst, src, dst); }

void __stdcall BindFramebuffer(GLenum target, GLuint framebuffer) {
    const bool draw = target == 0x8D40 || target == 0x8CA9; // GL_FRAMEBUFFER / GL_DRAW_FRAMEBUFFER
    const bool read = target == 0x8D40 || target == 0x8CA8; // GL_FRAMEBUFFER / GL_READ_FRAMEBUFFER
    const GLint fb = (GLint)framebuffer;
    if ((!draw || (g_DrawFbo.Valid && g_DrawFbo.V[0] == fb)) && (!read || (g_ReadFbo.Valid && g_ReadFbo.V[0] == fb)))
        return;
    ::glBindFramebuffer(target, framebuffer);
    if (draw) { g_DrawFbo.V[0] = fb; g_DrawFbo.Valid = true; }
    if (read) { g_ReadFbo.V[0] = fb; g_ReadFbo.Valid = true; }
}

void __stdcall DeleteFramebuffers(GLsizei n, const GLuint* framebuffers) {
    ::glDeleteFramebuffers(n, framebuffers);
    // Deleting a bound framebuffer reverts that binding to the default one.
    for (GLsizei i = 0; i < n; ++i) {
        if (framebuffers[i] == 0) continue;
        if (g_DrawFbo.Valid && g_DrawFbo.V[0] == (GLint)framebuffers[i]) g_DrawFbo.V[0] = 0;
        if (g_ReadFbo.Valid && g_ReadFbo.V[0] == (GLint)framebuffers[i]) g_ReadFbo.V[0] = 0;
    }
}

void __stdcall GetIntegerv(GLenum pname, GLint* data) {
    const GLint* v = nullptr;
    int count = 1;
    switch (pname) {
    case 0x0BA2: v = Read(g_Viewport, pname); count = 4; break; // GL_VIEWPORT
    case 0x0B74: v = Read(g_DepthFunc, pname); break;           // GL_DEPTH_FUNC
    case 0x0B72: v = Read(g_DepthMask, pname); break;           // GL_DEPTH_WRITEMASK
    case 0x0B45: v = Read(g_CullMode, pname); break;            // GL_CULL_FACE_MODE
    case 0x8CA6: v = Read(g_DrawFbo, pname); break;             // GL_(DRAW_)FRAMEBUFFER_BINDING
    case 0x8CAA: v = Read(g_ReadFbo, pname); break;             // GL_READ_FRAMEBUFFER_BINDING
    case 0x80C9: v = ReadBlend() + 0; break;                     // GL_BLEND_SRC_RGB
    case 0x80C8: v = ReadBlend() + 1; break;                     // GL_BLEND_DST_RGB
    case 0x80CB: v = ReadBlend() + 2; break;                     // GL_BLEND_SRC_ALPHA
    case 0x80CA: v = ReadBlend() + 3; break;                     // GL_BLEND_DST_ALPHA
    default:
        if (Cap* c = FindCap(pname)) { *data = CapOn(*c) ? 1 : 0; return; }
        ::glGetIntegerv(pname, data);
        return;
    }
    for (int i = 0; i < count; ++i) data[i] = v[i];
}

void __stdcall GetBooleanv(GLenum pname, GLboolean* data) {
    if (pname == 0x0B72) { *data = Read(g_DepthMask, pname)[0] ? GL_TRUE : GL_FALSE; return; }
    if (Cap* c = FindCap(pname)) { *data = CapOn(*c) ? GL_TRUE : GL_FALSE; return; }
    ::glGetBooleanv(pname, data);
}

} // namespace glshadow
