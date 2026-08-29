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
PFNGLUNIFORM1IPROC glUniform1i = nullptr;
PFNGLUNIFORM1FPROC glUniform1f = nullptr;
PFNGLUNIFORM3FPROC glUniform3f = nullptr;
PFNGLUNIFORM3FVPROC glUniform3fv = nullptr;
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

namespace {
void* LoadGLFunc(const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 || p == (void*)0x3 || p == (void*)-1) {
        HMODULE mod = LoadLibraryA("opengl32.dll");
        p = (void*)GetProcAddress(mod, name);
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
    LOAD(PFNGLUNIFORM1IPROC, glUniform1i)
    LOAD(PFNGLUNIFORM1FPROC, glUniform1f)
    LOAD(PFNGLUNIFORM3FPROC, glUniform3f)
    LOAD(PFNGLUNIFORM3FVPROC, glUniform3fv)
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
#undef LOAD
    return ok;
}
