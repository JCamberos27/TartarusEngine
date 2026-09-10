#include "GLStateCache.h"
#include "gl.h"
#include <array>
#ifndef NDEBUG
#include "Log.h"
// Not exposed by the engine's minimal gl.h loader (like GL_ONE / GL_RG elsewhere); only the
// debug desync check below needs it.
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#endif

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

#ifndef NDEBUG
// Debug-only desync detector (audit GL-202). A raw glBindTexture that bypasses this cache
// without a following Invalidate() leaves the shadow below out of step with real GL state, so a
// later BindTexture2D wrongly skips the real bind. Once per unit after each Invalidate(), the
// first skipped bind cross-checks the driver's actual GL_TEXTURE_BINDING_2D and logs on
// mismatch. Bounded to <=16 glGetIntegerv per frame; compiled out entirely in release.
std::array<bool, kMaxCachedTextureUnits> g_DebugUnitVerified{};

void DebugVerifyUnit(unsigned int unit, unsigned int expected) {
    if (g_DebugUnitVerified[unit]) return;
    g_DebugUnitVerified[unit] = true;
    GLint actual = 0;
    glActiveTexture(GL_TEXTURE0 + unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &actual);
    if ((unsigned int)actual != expected) {
        Log::Error("GLStateCache desync on texture unit " + std::to_string(unit) + ": cache says " +
                   std::to_string(expected) + ", GL has " + std::to_string(actual) +
                   " — a raw glBindTexture pass is missing its GLStateCache::Invalidate().");
    }
}
#endif

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
#ifndef NDEBUG
    g_DebugUnitVerified.fill(false);
#endif
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
#ifndef NDEBUG
        DebugVerifyUnit(unit, texture);
#endif
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
