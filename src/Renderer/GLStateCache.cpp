#include "GLStateCache.h"
#include "gl.h"
#include <array>

namespace {

unsigned int g_CurrentProgram = 0;
bool g_ProgramValid = false;

unsigned int g_CurrentVAO = 0;
bool g_VAOValid = false;

// GL core guarantees at least 16 combined texture units (4.6 mandates more) - this engine's own material
// binding (Model.cpp's BindMaterial) never uses more than 7, so 16 is comfortable headroom.
constexpr int kMaxCachedTextureUnits = 16;
std::array<unsigned int, kMaxCachedTextureUnits> g_BoundTextures{};
std::array<bool, kMaxCachedTextureUnits> g_TextureValid{};

GLStateCache::FrameStats g_Stats;

// #192: the material value-hash currently bound and the program it was bound under. Program is
// part of the key (a new pass rebinds) and Invalidate() clears it — which already runs at end
// of frame and after every raw-GL pass, so an edited material can't be wrongly skipped.
std::uint64_t g_BoundMaterialHash = 0;
unsigned int g_BoundMaterialProgram = 0;

} // namespace

namespace GLStateCache {

void Invalidate() {
    g_ProgramValid = false;
    g_TextureValid.fill(false);
    g_VAOValid = false;
    g_BoundMaterialHash = 0;
    g_BoundMaterialProgram = 0;
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

void BindVertexArray(unsigned int vao) {
    if (g_VAOValid && g_CurrentVAO == vao) {
        g_Stats.VaoBindsSkipped++;
        return;
    }
    glBindVertexArray(vao);
    g_CurrentVAO = vao;
    g_VAOValid = true;
    g_Stats.VaoBinds++;
}

const FrameStats& GetFrameStats() { return g_Stats; }
void ResetFrameStats() { g_Stats = FrameStats{}; }

bool MaterialAlreadyBound(std::uint64_t materialHash, unsigned int program) {
    if (program != 0 && program == g_BoundMaterialProgram && materialHash == g_BoundMaterialHash)
        return true;
    g_BoundMaterialHash = materialHash;
    g_BoundMaterialProgram = program;
    return false;
}

} // namespace GLStateCache
