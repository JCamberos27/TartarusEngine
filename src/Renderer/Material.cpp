// Material's per-frame statics, out of line so TartarusCore.dll holds the only copy (see Material.h).
#include "Material.h"

std::uint32_t Material::s_HashFrame = 1;
float Material::s_WindTime = 0.0f;
float Material::s_PrevWindTime = 0.0f;
glm::vec2 Material::s_WindDir{0.8f, 0.6f};
Material::WindField Material::s_Wind{};
