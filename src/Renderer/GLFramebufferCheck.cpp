#include "GLFramebufferCheck.h"
#include "gl.h"
#include "Log.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace GLFramebufferCheck {

namespace {

// Test seam (audit #358): set TARTARUS_FORCE_FBO_INCOMPLETE to make Complete() report failure
// without touching GL, so the graceful pass-disable / fallback paths can be exercised. An empty
// value fails every target; a non-empty value fails only owners whose name contains it as a
// substring (e.g. "Bloom", "Ssao", "IblProbe").
bool ForcedFail(const char* owner) {
    const char* v = std::getenv("TARTARUS_FORCE_FBO_INCOMPLETE");
    if (!v) return false;
    if (v[0] == '\0') return true;
    return owner && std::strstr(owner, v) != nullptr;
}
const char* StatusText(GLenum s) {
    switch (s) {
        case 0x8CD5: return "COMPLETE";
        case 0x8CD6: return "INCOMPLETE_ATTACHMENT";
        case 0x8CD7: return "INCOMPLETE_MISSING_ATTACHMENT";
        case 0x8CDB: return "INCOMPLETE_DRAW_BUFFER";
        case 0x8CDC: return "INCOMPLETE_READ_BUFFER";
        case 0x8CDD: return "UNSUPPORTED";
        case 0x8D56: return "INCOMPLETE_MULTISAMPLE";
        case 0x8DA8: return "INCOMPLETE_LAYER_TARGETS";
        case 0:      return "CHECK_FAILED (GL error / no context)";
        default:     return "UNKNOWN";
    }
}
} // namespace

bool Complete(const char* owner, unsigned int fbo, int width, int height) {
    if (ForcedFail(owner)) {
        Log::Error(std::string("Framebuffer forced-incomplete (TARTARUS_FORCE_FBO_INCOMPLETE): ") +
                   (owner ? owner : "(unknown)"));
        return false;
    }
    GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
    if (status == 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */) return true;
    Log::Error(std::string("Framebuffer incomplete: ") + (owner ? owner : "(unknown)") +
               "  " + std::to_string(width) + "x" + std::to_string(height) +
               "  status=" + StatusText(status) +
               " (0x" + [&]{ char b[8]; snprintf(b, sizeof(b), "%04X", status); return std::string(b); }() + ")");
    return false;
}

} // namespace GLFramebufferCheck
