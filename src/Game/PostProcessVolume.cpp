#include "PostProcessVolume.h"

#include "Components.h"
#include "Tonemapper.h"
#include "World.h"

#include <algorithm>
#include <vector>

float PostVolumeWeight(bool global, const glm::vec3& size, float blendDistance, float weight,
                       const glm::mat4& volumeWorld, const glm::vec3& cameraPos) {
    const float w = std::clamp(weight, 0.0f, 1.0f);
    if (global) return w;
    // Outside distance to the box, measured in the volume's local space then scaled back to
    // world units per axis, so a rotated / scaled box works.
    const glm::vec3 local = glm::vec3(glm::inverse(volumeWorld) * glm::vec4(cameraPos, 1.0f));
    const glm::vec3 half = glm::abs(size) * 0.5f;
    const glm::vec3 outside = glm::max(glm::abs(local) - half, glm::vec3(0.0f));
    const glm::vec3 axisScale(glm::length(glm::vec3(volumeWorld[0])), glm::length(glm::vec3(volumeWorld[1])),
                              glm::length(glm::vec3(volumeWorld[2])));
    const float dist = glm::length(outside * axisScale);
    if (dist <= 0.0f) return w;
    if (blendDistance <= 0.0f || dist >= blendDistance) return 0.0f;
    return w * (1.0f - dist / blendDistance);
}

void ApplyPostProcessVolumes(const World& world, const glm::vec3& cameraPos, PostSettings& p) {
    struct Active { const PostProcessVolumeComponent* V; float W; int Priority; };
    std::vector<Active> active;
    for (auto e : world.Registry.view<const PostProcessVolumeComponent, const TransformComponent>()) {
        if (world.Registry.all_of<InactiveTag>(e)) continue;
        const auto& v = world.Registry.get<const PostProcessVolumeComponent>(e);
        const float w = PostVolumeWeight(v.Global, v.Size, v.BlendDistance, v.Weight,
                                         world.GetCachedWorldTransform(e), cameraPos);
        if (w > 0.0f) active.push_back({&v, w, v.Priority});
    }
    std::stable_sort(active.begin(), active.end(), [](const Active& a, const Active& b) { return a.Priority < b.Priority; });
    auto blend = [](float& dst, bool on, float value, float w) { if (on) dst += (value - dst) * w; };
    for (const Active& a : active) {
        const auto& v = *a.V;
        blend(p.ExposureEV, v.OverrideExposure, v.ExposureEV, a.W);
        blend(p.Temperature, v.OverrideTemperature, v.Temperature, a.W);
        blend(p.Tint, v.OverrideTint, v.Tint, a.W);
        blend(p.Contrast, v.OverrideContrast, v.Contrast, a.W);
        blend(p.Saturation, v.OverrideSaturation, v.Saturation, a.W);
        blend(p.VignetteIntensity, v.OverrideVignette, v.Vignette, a.W);
        blend(p.ChromaticAberration, v.OverrideChromatic, v.ChromaticAberration, a.W);
        blend(p.FilmGrain, v.OverrideGrain, v.FilmGrain, a.W);
        // Bloom only brightens a glow that was computed this frame; with the scene's bloom off
        // there is no glow texture for a volume to turn up.
        if (p.BloomTexture) blend(p.BloomIntensity, v.OverrideBloom, v.BloomIntensity, a.W);
    }
}
