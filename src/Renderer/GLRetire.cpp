#include "GLRetire.h"
#include "gl.h"

#include <vector>

namespace {
std::vector<GLuint> g_Retired;
} // namespace

void GLRetire::Texture(unsigned int texture) {
    if (texture != 0) g_Retired.push_back(texture);
}

void GLRetire::Flush() {
    if (g_Retired.empty()) return;
    glDeleteTextures((GLsizei)g_Retired.size(), g_Retired.data());
    g_Retired.clear();
}

std::size_t GLRetire::Pending() { return g_Retired.size(); }
