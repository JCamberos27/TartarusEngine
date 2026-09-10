#pragma once

// One place every FBO-owning renderer class validates framebuffer completeness after it builds
// or resizes its attachments (audit #358). Previously only the two preview renderers called
// glCheckFramebufferStatus; SSAO, Bloom, the shadow maps, IBL, HDR and the plain Framebuffer
// did not, so an incomplete or unsupported attachment set silently produced a black / garbage
// pass instead of a diagnosable error.
//
// Logs a single readable line naming the owner, the dimensions, and the failure enum when the
// framebuffer is not GL_FRAMEBUFFER_COMPLETE. Returns true iff complete — callers store this as
// their validity flag so a dependent pass can be skipped rather than run against a broken target.
namespace GLFramebufferCheck {

// DSA: check a named framebuffer object without binding it.
bool Complete(const char* owner, unsigned int fbo, int width, int height);

} // namespace GLFramebufferCheck
