#include "GLDebug.h"
#include "Log.h"
#include "gl.h" // glEnable (core; already in the loader) — the debug enums below are passed as plain GLenums

#include <GLFW/glfw3.h>
#include <atomic>
#include <cstdlib>
#include <string>

namespace {

// KHR_debug constants. gl.h is a minimal core subset and doesn't define these;
// they're ordinary GLenum values, so passing them to glEnable / the loaded entry points is
// fine without the loader knowing about them.
constexpr GLenum kDebugOutput             = 0x92E0; // GL_DEBUG_OUTPUT
constexpr GLenum kDebugOutputSynchronous  = 0x8242; // GL_DEBUG_OUTPUT_SYNCHRONOUS
constexpr GLenum kSeverityHigh            = 0x9146; // GL_DEBUG_SEVERITY_HIGH
constexpr GLenum kSeverityMedium          = 0x9147; // GL_DEBUG_SEVERITY_MEDIUM
constexpr GLenum kSeverityLow             = 0x9148; // GL_DEBUG_SEVERITY_LOW
constexpr GLenum kSeverityNotification    = 0x826B; // GL_DEBUG_SEVERITY_NOTIFICATION
constexpr GLenum kDontCare                = 0x1100; // GL_DONT_CARE
constexpr GLenum kTypeError               = 0x824C; // GL_DEBUG_TYPE_ERROR

// Set true only once Init() actually wires up the callback (see IsEnabled()). Read by the
// --smoke-test harness in main.cpp to know whether ErrorCount() means anything.
bool gDebugActive = false;

// Set by GLDebug::ForceEnable() before Init() — makes Enabled() return true no matter the
// build config or env (audit #356, --smoke-test).
bool gForceEnable = false;

// Incremented from OnGlMessage for anything the smoke-test harness should treat as a real
// failure: a driver-flagged GL_DEBUG_TYPE_ERROR, or any GL_DEBUG_SEVERITY_HIGH message (some
// drivers report undefined-behaviour warnings as HIGH severity under a non-ERROR type instead).
// std::atomic since the callback can in principle fire off the main thread on some drivers.
std::atomic<int> gErrorCount{0};

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

void __stdcall OnGlMessage(GLenum /*source*/, GLenum type, GLuint id, GLenum severity,
                           GLsizei /*length*/, const GLchar* message, const void* /*userParam*/) {
    // Driver chatter ("buffer will use VIDEO memory", etc.) — never actionable, would flood
    // the Console.
    if (severity == kSeverityNotification) return;

    if (type == kTypeError || severity == kSeverityHigh) gErrorCount.fetch_add(1, std::memory_order_relaxed);

    std::string line = "GL[" + std::string(SeverityText(severity)) + "] (" + std::to_string(id) + ") " +
                       (message ? message : "(no message)");
    if (severity == kSeverityHigh) Log::Error(line);
    else                           Log::Warn(line);
}

bool Enabled() {
    if (gForceEnable) return true; // --smoke-test forced it on (audit #356)
#ifndef NDEBUG
    return true; // Debug builds always get it
#else
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996) // getenv is fine for a one-shot read-only debug toggle check
#endif
    const char* v = std::getenv("TARTARUS_GL_DEBUG");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return v && v[0] == '1';
#endif
}

} // namespace

void GLDebug::ForceEnable() { gForceEnable = true; }

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
    gDebugActive = true;
    Log::Info("GLDebug: OpenGL debug output enabled.");
}

bool GLDebug::IsEnabled() { return gDebugActive; }
int GLDebug::ErrorCount() { return gErrorCount.load(std::memory_order_relaxed); }
void GLDebug::ResetErrorCount() { gErrorCount.store(0, std::memory_order_relaxed); }
