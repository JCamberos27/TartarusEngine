#include "GLDebug.h"
#include "Log.h"
#include "gl.h" // glEnable (core; already in the loader) — the debug enums below are passed as plain GLenums

#include <GLFW/glfw3.h>
#include <cstdlib>
#include <string>

namespace {

// KHR_debug / GL 4.3 constants. gl.h is a minimal core-3.3 subset and doesn't define these;
// they're ordinary GLenum values, so passing them to glEnable / the loaded entry points is
// fine without the loader knowing about them.
constexpr GLenum kDebugOutput             = 0x92E0; // GL_DEBUG_OUTPUT
constexpr GLenum kDebugOutputSynchronous  = 0x8242; // GL_DEBUG_OUTPUT_SYNCHRONOUS
constexpr GLenum kSeverityHigh            = 0x9146; // GL_DEBUG_SEVERITY_HIGH
constexpr GLenum kSeverityMedium          = 0x9147; // GL_DEBUG_SEVERITY_MEDIUM
constexpr GLenum kSeverityLow             = 0x9148; // GL_DEBUG_SEVERITY_LOW
constexpr GLenum kSeverityNotification    = 0x826B; // GL_DEBUG_SEVERITY_NOTIFICATION
constexpr GLenum kDontCare                = 0x1100; // GL_DONT_CARE

using GLDEBUGPROC = void(__stdcall*)(GLenum source, GLenum type, GLuint id, GLenum severity,
                                     GLsizei length, const GLchar* message, const void* userParam);
using PFN_glDebugMessageCallback = void(__stdcall*)(GLDEBUGPROC callback, const void* userParam);
using PFN_glDebugMessageControl  = void(__stdcall*)(GLenum source, GLenum type, GLenum severity,
                                                    GLsizei count, const GLuint* ids, GLboolean enabled);

const char* SeverityText(GLenum severity) {
    switch (severity) {
        case kSeverityHigh:   return "HIGH";
        case kSeverityMedium: return "MEDIUM";
        case kSeverityLow:    return "LOW";
        default:              return "NOTE";
    }
}

void __stdcall OnGlMessage(GLenum /*source*/, GLenum /*type*/, GLuint id, GLenum severity,
                           GLsizei /*length*/, const GLchar* message, const void* /*userParam*/) {
    // Driver chatter ("buffer will use VIDEO memory", etc.) — never actionable, would flood
    // the Console.
    if (severity == kSeverityNotification) return;

    std::string line = "GL[" + std::string(SeverityText(severity)) + "] (" + std::to_string(id) + ") " +
                       (message ? message : "(no message)");
    if (severity == kSeverityHigh) Log::Error(line);
    else                           Log::Warn(line);
}

bool Enabled() {
#ifndef NDEBUG
    return true; // Debug builds always get it
#else
    const char* v = std::getenv("TARTARUS_GL_DEBUG");
    return v && v[0] == '1';
#endif
}

} // namespace

void GLDebug::Init() {
    if (!Enabled()) return;

    auto setCallback = reinterpret_cast<PFN_glDebugMessageCallback>(glfwGetProcAddress("glDebugMessageCallback"));
    auto setControl  = reinterpret_cast<PFN_glDebugMessageControl>(glfwGetProcAddress("glDebugMessageControl"));
    if (!setCallback) {
        Log::Warn("GLDebug: this GL context has no glDebugMessageCallback (needs KHR_debug / GL 4.3) - "
                  "debug output disabled.");
        return;
    }

    glEnable(kDebugOutput);
    glEnable(kDebugOutputSynchronous); // deliver on the call that caused it, so a stack trace lands in the right place
    setCallback(&OnGlMessage, nullptr);
    if (setControl) {
        // Start from "everything on", then let OnGlMessage drop notifications itself.
        setControl(kDontCare, kDontCare, kDontCare, 0, nullptr, GL_TRUE);
    }
    Log::Info("GLDebug: OpenGL debug output enabled.");
}
