#pragma once

// Optional OpenGL debug-output plumbing. Routes the driver's own validation / performance /
// undefined-behaviour messages into the engine Log, so a bad GL call surfaces as a readable
// Console line instead of silently doing nothing (or corrupting a later frame).
//
// Compiled into every build but INERT unless explicitly switched on:
//   * a Debug build (NDEBUG not defined), or
//   * the environment variable TARTARUS_GL_DEBUG=1 on any build.
// With neither, GLDebug::Init() returns immediately and nothing about the render path changes.
//
// glDebugMessageCallback / glDebugMessageControl are loaded dynamically via glfwGetProcAddress
// here — this deliberately does NOT extend the hand-rolled gl.h loader. If the running context
// doesn't expose KHR_debug / GL 4.3 debug output, Init() logs one line and does nothing else.
namespace GLDebug {

// Force debug output on for this process regardless of build config or TARTARUS_GL_DEBUG.
// Call BEFORE Init(). Used by --smoke-test so its GL error count is never structurally zero
// (audit #356).
void ForceEnable();

// Call once, after the GL context and loader are ready (i.e. just after the Window is built).
// Safe to call when disabled — it just returns.
void Init();

// True once Init() has actually wired up the callback (Debug build, or TARTARUS_GL_DEBUG=1,
// AND the context exposes KHR_debug). False means ErrorCount() below can never move — there's
// no debug output to count errors from.
bool IsEnabled();

// Running count of GL_DEBUG_TYPE_ERROR and GL_DEBUG_SEVERITY_HIGH messages seen since startup
// (or since the last ResetErrorCount()). Used by the --smoke-test harness (main.cpp) to detect
// whether a scene's load+render introduced any NEW driver-reported errors: it snapshots this
// before a scene and diffs against it after N frames, rather than needing an absolute-zero
// baseline across the whole run.
int ErrorCount();
void ResetErrorCount();

} // namespace GLDebug
