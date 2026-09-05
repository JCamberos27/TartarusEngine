// Minimal hand-rolled OpenGL 4.6 core loader (Windows).
// Loads only the subset of GL used by this engine's renderer. The engine still targets a small,
// conservative slice of the API — the 4.6 context mainly buys guaranteed KHR_debug, core
// anisotropic filtering, and headroom for future features — so this stays deliberately short.
#pragma once
#include <cstddef>

#define GL_TRUE 1
#define GL_FALSE 0
#define GL_DEPTH_TEST 0x0B71
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_CULL_FACE 0x0B44
#define GL_BACK 0x0405
#define GL_FRONT 0x0404
#define GL_CCW 0x0901
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_TRIANGLES 0x0004
#define GL_LINES 0x0001
#define GL_FLOAT 0x1406
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_BYTE 0x1401
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_LEQUAL 0x0203
#define GL_LESS 0x0201
#define GL_DEPTH_FUNC 0x0B74
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_FILL 0x1B02
#define GL_LINE 0x1B01
#define GL_FRONT_AND_BACK 0x0408
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_RGB 0x1907
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_LOD_BIAS 0x8501
// Anisotropic filtering — an extension since 2000, promoted to core in GL 4.6.
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_REPEAT 0x2901
#define GL_SRGB8 0x8C41
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_MULTISAMPLE 0x809D
#define GL_SCISSOR_TEST 0x0C11
#define GL_STENCIL_TEST 0x0B90
#define GL_PRIMITIVE_RESTART 0x8F9D
#define GL_FUNC_ADD 0x8006
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_VERSION 0x1F02
#define GL_RGB8 0x8051
#define GL_RGBA8 0x8058
#define GL_RED 0x1903
#define GL_R8 0x8229
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_NEAREST 0x2600
#define GL_VIEWPORT 0x0BA2
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_STENCIL 0x84F9
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_RENDERBUFFER 0x8D41
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_MAX_SAMPLES 0x8D57

// ---- Lighting / HDR overhaul: extended core (GL 4.3-4.6) --------------------------------
// Buffer objects for UBO/SSBO light data + immutable (DSA) storage.
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_PERSISTENT_BIT 0x0040
#define GL_MAP_COHERENT_BIT 0x0080
#define GL_DYNAMIC_STORAGE_BIT 0x0100
// Floating-point / HDR internal formats.
#define GL_RGBA16F 0x881A
#define GL_RGBA32F 0x8814
#define GL_RGB16F 0x881B
#define GL_RGB32F 0x8815
#define GL_RG16F 0x822F
#define GL_R16F 0x822D
#define GL_R11F_G11F_B10F 0x8C3A
#define GL_HALF_FLOAT 0x140B
#define GL_DEPTH_COMPONENT32F 0x8CAC
// Multisample + layered texture targets (MSAA HDR target, shadow-map arrays, cubemaps).
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE_CUBE_MAP_ARRAY 0x9009
#define GL_TEXTURE_CUBE_MAP_SEAMLESS 0x884F
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#define GL_NONE 0
#define GL_DEPTH_COMPONENT 0x1902
// Extra colour attachments for the future G-buffer-ish passes (bloom, SSAO, motion).
#define GL_COLOR_ATTACHMENT1 0x8CE1
#define GL_COLOR_ATTACHMENT2 0x8CE2
#define GL_COLOR_ATTACHMENT3 0x8CE3
#define GL_FRAMEBUFFER_SRGB 0x8DB9

// Compute shaders + memory barriers — clustered light-culling pass (#120).
#define GL_COMPUTE_SHADER 0x91B9
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#define GL_BUFFER_UPDATE_BARRIER_BIT 0x00000200
#define GL_ALL_BARRIER_BITS 0xFFFFFFFF
#define GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS 0x90EB

// GPU timer queries — Profiler's GPU-side timing (#197).
#define GL_TIME_ELAPSED 0x88BF
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned long long GLuint64;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef float GLfloat;
typedef double GLdouble;
typedef unsigned int GLbitfield;

// --- core GL (already exported by opengl32.dll on Windows) ---
extern "C" {
void __stdcall glClear(GLbitfield mask);
void __stdcall glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void __stdcall glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void __stdcall glEnable(GLenum cap);
void __stdcall glDisable(GLenum cap);
void __stdcall glDepthFunc(GLenum func);
void __stdcall glDepthMask(GLboolean flag);
void __stdcall glCullFace(GLenum mode);
void __stdcall glPolygonOffset(GLfloat factor, GLfloat units);
void __stdcall glFrontFace(GLenum mode);
void __stdcall glDrawArrays(GLenum mode, GLint first, GLsizei count);
void __stdcall glPolygonMode(GLenum face, GLenum mode);
void __stdcall glBlendFunc(GLenum sfactor, GLenum dfactor);
void __stdcall glGenTextures(GLsizei n, GLuint* textures);
void __stdcall glBindTexture(GLenum target, GLuint texture);
void __stdcall glTexParameteri(GLenum target, GLenum pname, GLint param);
void __stdcall glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void __stdcall glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
void __stdcall glDeleteTextures(GLsizei n, const GLuint* textures);
void __stdcall glGetIntegerv(GLenum pname, GLint* data);
GLboolean __stdcall glIsEnabled(GLenum cap);
const GLubyte* __stdcall glGetString(GLenum name);
void __stdcall glPixelStorei(GLenum pname, GLint param);
void __stdcall glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void __stdcall glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
void __stdcall glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
}

// --- functions requiring wglGetProcAddress (GL >= 1.2 extras used here) ---
typedef void (__stdcall* PFNGLGENVERTEXARRAYSPROC)(GLsizei, GLuint*);
typedef void (__stdcall* PFNGLBINDVERTEXARRAYPROC)(GLuint);
typedef void (__stdcall* PFNGLDELETEVERTEXARRAYSPROC)(GLsizei, const GLuint*);
typedef void (__stdcall* PFNGLGENBUFFERSPROC)(GLsizei, GLuint*);
typedef void (__stdcall* PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void (__stdcall* PFNGLBUFFERDATAPROC)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void (__stdcall* PFNGLBUFFERSUBDATAPROC)(GLenum, GLintptr, GLsizeiptr, const void*);
typedef void (__stdcall* PFNGLDELETEBUFFERSPROC)(GLsizei, const GLuint*);
typedef void (__stdcall* PFNGLVERTEXATTRIBPOINTERPROC)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void (__stdcall* PFNGLVERTEXATTRIBIPOINTERPROC)(GLuint, GLint, GLenum, GLsizei, const void*);
typedef void (__stdcall* PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef GLuint (__stdcall* PFNGLCREATESHADERPROC)(GLenum);
typedef void (__stdcall* PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void (__stdcall* PFNGLCOMPILESHADERPROC)(GLuint);
typedef void (__stdcall* PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint*);
typedef void (__stdcall* PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void (__stdcall* PFNGLDELETESHADERPROC)(GLuint);
typedef GLuint (__stdcall* PFNGLCREATEPROGRAMPROC)(void);
typedef void (__stdcall* PFNGLATTACHSHADERPROC)(GLuint, GLuint);
typedef void (__stdcall* PFNGLLINKPROGRAMPROC)(GLuint);
typedef void (__stdcall* PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint*);
typedef void (__stdcall* PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void (__stdcall* PFNGLUSEPROGRAMPROC)(GLuint);
typedef void (__stdcall* PFNGLDELETEPROGRAMPROC)(GLuint);
typedef GLint (__stdcall* PFNGLGETUNIFORMLOCATIONPROC)(GLuint, const GLchar*);
typedef void (__stdcall* PFNGLUNIFORMMATRIX4FVPROC)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void (__stdcall* PFNGLUNIFORM2FPROC)(GLint, GLfloat, GLfloat);
typedef void (__stdcall* PFNGLUNIFORM1IPROC)(GLint, GLint);
typedef void (__stdcall* PFNGLUNIFORM1FPROC)(GLint, GLfloat);
typedef void (__stdcall* PFNGLUNIFORM3FPROC)(GLint, GLfloat, GLfloat, GLfloat);
typedef void (__stdcall* PFNGLUNIFORM3FVPROC)(GLint, GLsizei, const GLfloat*);
typedef void (__stdcall* PFNGLUNIFORM4FPROC)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (__stdcall* PFNGLACTIVETEXTUREPROC)(GLenum);
typedef void (__stdcall* PFNGLGENERATEMIPMAPPROC)(GLenum);
typedef void (__stdcall* PFNGLDRAWELEMENTSPROC)(GLenum, GLsizei, GLenum, const void*);
typedef void (__stdcall* PFNGLDRAWELEMENTSBASEVERTEXPROC)(GLenum, GLsizei, GLenum, const void*, GLint);
typedef GLint (__stdcall* PFNGLGETATTRIBLOCATIONPROC)(GLuint, const GLchar*);
typedef void (__stdcall* PFNGLDETACHSHADERPROC)(GLuint, GLuint);
typedef void (__stdcall* PFNGLBINDSAMPLERPROC)(GLuint, GLuint);
typedef void (__stdcall* PFNGLBLENDEQUATIONPROC)(GLenum);
typedef void (__stdcall* PFNGLBLENDEQUATIONSEPARATEPROC)(GLenum, GLenum);
typedef void (__stdcall* PFNGLBLENDFUNCSEPARATEPROC)(GLenum, GLenum, GLenum, GLenum);
typedef void (__stdcall* PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint*);
typedef void (__stdcall* PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef void (__stdcall* PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (__stdcall* PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef void (__stdcall* PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint*);
typedef void (__stdcall* PFNGLGENRENDERBUFFERSPROC)(GLsizei, GLuint*);
typedef void (__stdcall* PFNGLBINDRENDERBUFFERPROC)(GLenum, GLuint);
typedef void (__stdcall* PFNGLRENDERBUFFERSTORAGEPROC)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (__stdcall* PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum, GLenum, GLenum, GLuint);
typedef void (__stdcall* PFNGLDELETERENDERBUFFERSPROC)(GLsizei, const GLuint*);
typedef void (__stdcall* PFNGLBLITFRAMEBUFFERPROC)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

// ---- Lighting / HDR overhaul: extended entry points -----------------------------------
typedef const GLubyte* (__stdcall* PFNGLGETSTRINGIPROC)(GLenum, GLuint);
typedef void (__stdcall* PFNGLBINDBUFFERBASEPROC)(GLenum, GLuint, GLuint);
typedef void (__stdcall* PFNGLBINDBUFFERRANGEPROC)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
typedef void (__stdcall* PFNGLDRAWBUFFERSPROC)(GLsizei, const GLenum*);
typedef void (__stdcall* PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (__stdcall* PFNGLTEXIMAGE2DMULTISAMPLEPROC)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean);
// Direct State Access (GL 4.5) — buffers.
typedef void (__stdcall* PFNGLCREATEBUFFERSPROC)(GLsizei, GLuint*);
typedef void (__stdcall* PFNGLNAMEDBUFFERSTORAGEPROC)(GLuint, GLsizeiptr, const void*, GLbitfield);
typedef void (__stdcall* PFNGLNAMEDBUFFERSUBDATAPROC)(GLuint, GLintptr, GLsizeiptr, const void*);
// DSA — textures.
typedef void (__stdcall* PFNGLCREATETEXTURESPROC)(GLenum, GLsizei, GLuint*);
typedef void (__stdcall* PFNGLTEXTURESTORAGE2DPROC)(GLuint, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (__stdcall* PFNGLTEXTURESTORAGE3DPROC)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);
typedef void (__stdcall* PFNGLTEXTURESTORAGE2DMULTISAMPLEPROC)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLboolean);
typedef void (__stdcall* PFNGLTEXTURESUBIMAGE2DPROC)(GLuint, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
typedef void (__stdcall* PFNGLTEXTURESUBIMAGE3DPROC)(GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*);
typedef void (__stdcall* PFNGLTEXTUREPARAMETERIPROC)(GLuint, GLenum, GLint);
typedef void (__stdcall* PFNGLTEXTUREPARAMETERFVPROC)(GLuint, GLenum, const GLfloat*);
typedef void (__stdcall* PFNGLGENERATETEXTUREMIPMAPPROC)(GLuint);
typedef void (__stdcall* PFNGLBINDTEXTUREUNITPROC)(GLuint, GLuint);
// DSA — framebuffers.
typedef void (__stdcall* PFNGLCREATEFRAMEBUFFERSPROC)(GLsizei, GLuint*);
typedef void (__stdcall* PFNGLNAMEDFRAMEBUFFERTEXTUREPROC)(GLuint, GLenum, GLuint, GLint);
typedef void (__stdcall* PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC)(GLuint, GLenum, GLuint, GLint, GLint);
typedef void (__stdcall* PFNGLNAMEDFRAMEBUFFERRENDERBUFFERPROC)(GLuint, GLenum, GLenum, GLuint);
typedef void (__stdcall* PFNGLNAMEDFRAMEBUFFERDRAWBUFFERSPROC)(GLuint, GLsizei, const GLenum*);
typedef GLenum (__stdcall* PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC)(GLuint, GLenum);
typedef void (__stdcall* PFNGLBLITNAMEDFRAMEBUFFERPROC)(GLuint, GLuint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (__stdcall* PFNGLCLEARNAMEDFRAMEBUFFERFVPROC)(GLuint, GLenum, GLint, const GLfloat*);
// DSA — vertex arrays (GL 4.5). Used by ModelMesh so building a mesh mid-frame no longer
// binds/mutates the renderer's current VAO + array/element-buffer state (#97).
typedef void (__stdcall* PFNGLCREATEVERTEXARRAYSPROC)(GLsizei, GLuint*);
typedef void (__stdcall* PFNGLENABLEVERTEXARRAYATTRIBPROC)(GLuint, GLuint);
typedef void (__stdcall* PFNGLVERTEXARRAYATTRIBFORMATPROC)(GLuint, GLuint, GLint, GLenum, GLboolean, GLuint);
typedef void (__stdcall* PFNGLVERTEXARRAYATTRIBIFORMATPROC)(GLuint, GLuint, GLint, GLenum, GLuint);
typedef void (__stdcall* PFNGLVERTEXARRAYATTRIBBINDINGPROC)(GLuint, GLuint, GLuint);
typedef void (__stdcall* PFNGLVERTEXARRAYVERTEXBUFFERPROC)(GLuint, GLuint, GLuint, GLintptr, GLsizei);
typedef void (__stdcall* PFNGLVERTEXARRAYELEMENTBUFFERPROC)(GLuint, GLuint);
// Compute dispatch + memory barrier (GL 4.3) — clustered light culling (#120).
typedef void (__stdcall* PFNGLDISPATCHCOMPUTEPROC)(GLuint, GLuint, GLuint);
typedef void (__stdcall* PFNGLMEMORYBARRIERPROC)(GLbitfield);

extern PFNGLGENVERTEXARRAYSPROC glGenVertexArrays;
extern PFNGLBINDVERTEXARRAYPROC glBindVertexArray;
extern PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays;
extern PFNGLGENBUFFERSPROC glGenBuffers;
extern PFNGLBINDBUFFERPROC glBindBuffer;
extern PFNGLBUFFERDATAPROC glBufferData;
extern PFNGLBUFFERSUBDATAPROC glBufferSubData;
extern PFNGLDELETEBUFFERSPROC glDeleteBuffers;
extern PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer;
extern PFNGLVERTEXATTRIBIPOINTERPROC glVertexAttribIPointer;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
extern PFNGLCREATESHADERPROC glCreateShader;
extern PFNGLSHADERSOURCEPROC glShaderSource;
extern PFNGLCOMPILESHADERPROC glCompileShader;
extern PFNGLGETSHADERIVPROC glGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog;
extern PFNGLDELETESHADERPROC glDeleteShader;
extern PFNGLCREATEPROGRAMPROC glCreateProgram;
extern PFNGLATTACHSHADERPROC glAttachShader;
extern PFNGLLINKPROGRAMPROC glLinkProgram;
extern PFNGLGETPROGRAMIVPROC glGetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog;
extern PFNGLUSEPROGRAMPROC glUseProgram;
extern PFNGLDELETEPROGRAMPROC glDeleteProgram;
extern PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation;
extern PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv;
extern PFNGLUNIFORM2FPROC glUniform2f;
extern PFNGLUNIFORM1IPROC glUniform1i;
extern PFNGLUNIFORM1FPROC glUniform1f;
extern PFNGLUNIFORM3FPROC glUniform3f;
extern PFNGLUNIFORM3FVPROC glUniform3fv;
extern PFNGLUNIFORM4FPROC glUniform4f;
extern PFNGLACTIVETEXTUREPROC glActiveTexture;
extern PFNGLGENERATEMIPMAPPROC glGenerateMipmap;
extern PFNGLDRAWELEMENTSPROC glDrawElements;
extern PFNGLDRAWELEMENTSBASEVERTEXPROC glDrawElementsBaseVertex;
extern PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation;
extern PFNGLDETACHSHADERPROC glDetachShader;
extern PFNGLBINDSAMPLERPROC glBindSampler;
extern PFNGLBLENDEQUATIONPROC glBlendEquation;
extern PFNGLBLENDEQUATIONSEPARATEPROC glBlendEquationSeparate;
extern PFNGLBLENDFUNCSEPARATEPROC glBlendFuncSeparate;
extern PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
extern PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
extern PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers;
extern PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer;
extern PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
extern PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers;
extern PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer;

// ---- Lighting / HDR overhaul: extended entry points -----------------------------------
extern PFNGLGETSTRINGIPROC glGetStringi;
extern PFNGLBINDBUFFERBASEPROC glBindBufferBase;
extern PFNGLBINDBUFFERRANGEPROC glBindBufferRange;
extern PFNGLDRAWBUFFERSPROC glDrawBuffers;
extern PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC glRenderbufferStorageMultisample;
extern PFNGLTEXIMAGE2DMULTISAMPLEPROC glTexImage2DMultisample;
extern PFNGLCREATEBUFFERSPROC glCreateBuffers;
extern PFNGLNAMEDBUFFERSTORAGEPROC glNamedBufferStorage;
extern PFNGLNAMEDBUFFERSUBDATAPROC glNamedBufferSubData;
extern PFNGLCREATETEXTURESPROC glCreateTextures;
extern PFNGLTEXTURESTORAGE2DPROC glTextureStorage2D;
extern PFNGLTEXTURESTORAGE3DPROC glTextureStorage3D;
extern PFNGLTEXTURESTORAGE2DMULTISAMPLEPROC glTextureStorage2DMultisample;
extern PFNGLTEXTURESUBIMAGE2DPROC glTextureSubImage2D;
extern PFNGLTEXTURESUBIMAGE3DPROC glTextureSubImage3D;
extern PFNGLTEXTUREPARAMETERIPROC glTextureParameteri;
extern PFNGLTEXTUREPARAMETERFVPROC glTextureParameterfv;
extern PFNGLGENERATETEXTUREMIPMAPPROC glGenerateTextureMipmap;
extern PFNGLBINDTEXTUREUNITPROC glBindTextureUnit;
extern PFNGLCREATEFRAMEBUFFERSPROC glCreateFramebuffers;
extern PFNGLNAMEDFRAMEBUFFERTEXTUREPROC glNamedFramebufferTexture;
extern PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC glNamedFramebufferTextureLayer;
extern PFNGLNAMEDFRAMEBUFFERRENDERBUFFERPROC glNamedFramebufferRenderbuffer;
extern PFNGLNAMEDFRAMEBUFFERDRAWBUFFERSPROC glNamedFramebufferDrawBuffers;
extern PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC glCheckNamedFramebufferStatus;
extern PFNGLBLITNAMEDFRAMEBUFFERPROC glBlitNamedFramebuffer;
extern PFNGLCLEARNAMEDFRAMEBUFFERFVPROC glClearNamedFramebufferfv;
extern PFNGLCREATEVERTEXARRAYSPROC glCreateVertexArrays;
extern PFNGLENABLEVERTEXARRAYATTRIBPROC glEnableVertexArrayAttrib;
extern PFNGLVERTEXARRAYATTRIBFORMATPROC glVertexArrayAttribFormat;
extern PFNGLVERTEXARRAYATTRIBIFORMATPROC glVertexArrayAttribIFormat;
extern PFNGLVERTEXARRAYATTRIBBINDINGPROC glVertexArrayAttribBinding;
extern PFNGLVERTEXARRAYVERTEXBUFFERPROC glVertexArrayVertexBuffer;
extern PFNGLVERTEXARRAYELEMENTBUFFERPROC glVertexArrayElementBuffer;
extern PFNGLDISPATCHCOMPUTEPROC glDispatchCompute;
extern PFNGLMEMORYBARRIERPROC glMemoryBarrier;

// GPU timer queries — Profiler's GPU-side timing (#197).
typedef void (__stdcall* PFNGLGENQUERIESPROC)(GLsizei, GLuint*);
typedef void (__stdcall* PFNGLDELETEQUERIESPROC)(GLsizei, const GLuint*);
typedef void (__stdcall* PFNGLBEGINQUERYPROC)(GLenum, GLuint);
typedef void (__stdcall* PFNGLENDQUERYPROC)(GLenum);
typedef void (__stdcall* PFNGLGETQUERYOBJECTIVPROC)(GLuint, GLenum, GLint*);
typedef void (__stdcall* PFNGLGETQUERYOBJECTUI64VPROC)(GLuint, GLenum, GLuint64*);
extern PFNGLGENQUERIESPROC glGenQueries;
extern PFNGLDELETEQUERIESPROC glDeleteQueries;
extern PFNGLBEGINQUERYPROC glBeginQuery;
extern PFNGLENDQUERYPROC glEndQuery;
extern PFNGLGETQUERYOBJECTIVPROC glGetQueryObjectiv;
extern PFNGLGETQUERYOBJECTUI64VPROC glGetQueryObjectui64v;

// Call once after a GL context is current (e.g. right after glfwMakeContextCurrent).
bool GLLoader_Init();
