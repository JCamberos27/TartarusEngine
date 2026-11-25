// Material's per-frame statics, out of line so TartarusCore.dll holds the only copy (see Material.h).
#include "Material.h"
#include "RenderSettingsEpoch.h"
#include "Texture.h"

std::uint32_t Material::s_HashFrame = 1;
float Material::s_WindTime = 0.0f;
float Material::s_PrevWindTime = 0.0f;
bool Material::s_WindPinned = false;
glm::vec2 Material::s_WindDir{0.8f, 0.6f};
Material::WindField Material::s_Wind{};

std::uint64_t Material::DepthKey() const {
    const float scalars[] = {AlphaCutoff, UVTiling.x, UVTiling.y, UVOffset.x, UVOffset.y,
                             WindStrength, WindFrequency, WindFlutter, WindHeight};
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&h](std::uint64_t w) { h = (h ^ w) * 0x100000001b3ull; h ^= h >> 29; };
    for (const float f : scalars) { std::uint32_t w; std::memcpy(&w, &f, 4); mix(w); }
    mix((std::uint64_t)(std::uintptr_t)AlbedoMap.get());
    const bool cutoutReady = AlphaClip && AlbedoMap && AlbedoMap->IsValid();
    mix((AlphaClip ? 1u : 0u) | (Foliage ? 2u : 0u) | (DoubleSided ? 4u : 0u) | (UseVertexColor ? 8u : 0u) |
        (cutoutReady ? 16u : 0u));
    return h;
}

void Material::WatchDepthFields() const {
    const std::uint64_t key = DepthKey();
    if (m_DepthWatch.Seen && m_DepthWatch.Key != key) NoteRenderSettingsChanged(); // wakes quiet local lights (G4-c)
    m_DepthWatch.Key = key;
    m_DepthWatch.Seen = true;
}
