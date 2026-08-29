#include "GLStateCache.h"
#include "gl.h"
#include <array>

namespace {

unsigned int g_CurrentProgram = 0;
bool g_ProgramValid = false;

// GL 3.3 core guarantees at least 16 combined texture units - this engine's own material
// binding (Model.cpp's BindMaterial) never uses more than 7, so 16 is comfortable headroom.
constexpr int kMaxCachedTextureUnits = 16;
std::array<unsigned int, kMaxCachedTextureUnits> g_BoundTextures{};
std::array<bool, kMaxCachedTextureUnits> g_TextureValid{};

GLStateCache::FrameStats g_Stats;

} // namespace

namespace GLStateCache {

void Invalidate() {
    g_ProgramValid = false;
    g_TextureValid.fill(false);
}

void UseProgram(unsigned int program) {
    if (g_ProgramValid && g_CurrentProgram == program) {
        g_Stats.ProgramBindsSkipped++;
        return;
    }
    glUseProgram(program);
    g_CurrentProgram = program;
    g_ProgramValid = true;
    g_Stats.ProgramBinds++;
}

void BindTexture2D(unsigned int unit, unsigned int texture) {
    // An out-of-range unit falls back to an uncached direct call rather than indexing past the
    // array - this engine never actually uses more than ~7 units, so this branch is a safety
    // net, not a real code path.
    if (unit >= (unsigned int)kMaxCachedTextureUnits) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, texture);
        g_Stats.TextureBinds++;
        return;
    }
    if (g_TextureValid[unit] && g_BoundTextures[unit] == texture) {
        g_Stats.TextureBindsSkipped++;
        return;
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture);
    g_BoundTextures[unit] = texture;
    g_TextureValid[unit] = true;
    g_Stats.TextureBinds++;
}

const FrameStats& GetFrameStats() { return g_Stats; }
void ResetFrameStats() { g_Stats = FrameStats{}; }

} // namespace GLStateCache
