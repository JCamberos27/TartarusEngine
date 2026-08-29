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

// Call once, after the GL context and loader are ready (i.e. just after the Window is built).
// Safe to call when disabled — it just returns.
void Init();

} // namespace GLDebug
