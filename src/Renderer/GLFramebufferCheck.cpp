#include "GLFramebufferCheck.h"
#include "gl.h"
#include "Log.h"

#include <string>

namespace GLFramebufferCheck {

namespace {
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
    GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
    if (status == 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */) return true;
    Log::Error(std::string("Framebuffer incomplete: ") + (owner ? owner : "(unknown)") +
               "  " + std::to_string(width) + "x" + std::to_string(height) +
               "  status=" + StatusText(status) +
               " (0x" + [&]{ char b[8]; snprintf(b, sizeof(b), "%04X", status); return std::string(b); }() + ")");
    return false;
}

} // namespace GLFramebufferCheck
