// Material's per-frame statics, out of line so TartarusCore.dll holds the only copy (see Material.h).
#include "Material.h"
#include "RenderSettingsEpoch.h"

std::uint32_t Material::s_HashFrame = 1;
float Material::s_WindTime = 0.0f;
float Material::s_PrevWindTime = 0.0f;
bool Material::s_WindPinned = false;
glm::vec2 Material::s_WindDir{0.8f, 0.6f};
Material::WindField Material::s_Wind{};

void Material::WatchDepthFields() const {
    const std::uint64_t key = DepthKey();
    if (m_DepthWatch.Seen && m_DepthWatch.Key != key) NoteRenderSettingsChanged(); // wakes quiet local lights (G4-c)
    m_DepthWatch.Key = key;
    m_DepthWatch.Seen = true;
}
