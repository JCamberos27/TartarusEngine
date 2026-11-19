#include "PointShadowMap.h"
#include "Log.h"
#include "gl.h"

#include <algorithm>
#include <string>

void PointShadowMap::Configure(int resolution, int cubes) {
    resolution = std::clamp(resolution, 256, 2048);
    cubes = std::clamp(cubes, 1, kMaxPoints);
    if (m_Target.Texture() != 0 && resolution == m_Resolution && cubes == m_Cubes) return;

    m_Resolution = resolution;
    m_Cubes = cubes;

    // A cube-map array is a 2D array with depth = numCubes * 6; layer = cube*6 + face. The static casters' depth
    // (never sampled, only copied from) is made on first use: DepthArrayTarget::EnsureStatic.
    DepthArrayTarget::Desc d;
    d.target = GL_TEXTURE_CUBE_MAP_ARRAY;
    d.resolution = resolution;
    d.layers = cubes * 6;
    d.borderClamp = false;
    d.liveName = "PointShadowMap (live)";
    d.staticName = "PointShadowMap (static cache)";
    m_Target.Create(d, this);

    if (!m_Target.Fbo() || !m_Target.Texture())
        Log::Error("PointShadowMap: failed to create depth cube array (" + std::to_string(resolution) + ")");
}

namespace {
int FaceLayer(int slot, int face, int cubes) { return std::clamp(slot, 0, std::max(cubes, 1) - 1) * 6 + std::clamp(face, 0, 5); }
}

// The static-caster cache is allocated when a face first needs it: a scene with no shadowed point light never pays for it.
void PointShadowMap::BeginStaticFace(int slot, int face) const { m_Target.BeginStatic(FaceLayer(slot, face, m_Cubes)); }

void PointShadowMap::BeginFaceFromStatic(int slot, int face) const {
    m_Target.BeginFromStatic(FaceLayer(slot, face, m_Cubes), 0, 0, m_Resolution, m_Resolution);
}
