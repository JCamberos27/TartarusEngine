#include "ShaderVariant.h"
#include "Material.h"

#include <cstring>

ShaderVariantKey ShaderVariantKeyFor(const Material& m, const ShaderAsset& sa) {
    const std::vector<std::string>& kw = sa.Keywords();
    ShaderVariantKey key = 0;

    auto bit = [&](const char* name, bool on) {
        if (!on) return;
        for (size_t i = 0; i < kw.size() && i < 32; ++i)
            if (kw[i] == name) { key |= (ShaderVariantKey{1} << i); return; }
    };

    bit("_CLEARCOAT",         m.ClearCoat > 0.0f);
    bit("_ANISO",             m.Anisotropy != 0.0f);
    bit("_SHEEN",             m.Sheen.x > 0.0f || m.Sheen.y > 0.0f || m.Sheen.z > 0.0f);
    bit("_SUBSURFACE",        m.SubsurfaceEnabled);
    bit("_TRANSMISSION",      m.TransmissionStrength > 0.0f);
    bit("_REFLECTION_PROBES", m.ReflectionProbes);

    return key;
}
