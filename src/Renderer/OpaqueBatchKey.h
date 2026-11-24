#pragma once

#include <cstdint>

// GPU-driven opaque draws (SceneViewPass::BuildBatches): instance groups whose sub-meshes bind alike share one multi-draw,
// and the per-draw hook (SceneRenderer::ViewPass::PerDraw) binds the batch LEAD's per-object state once for all of them.
// So the key that merges groups must hold everything that hook binds. It once packed the probe selection into bits
// 40-63 (losing its top 8 bits) and left the GI volume mask out: groups in different GI Volumes shared a batch and were
// shaded with the lead's mask, and which group led followed pointer order, so the baked GI changed from launch to launch
// (GI Phase 1, W3). Hashed, not bit-packed, so nothing is truncated.
namespace OpaqueBatchKey {

inline std::uint64_t Mix(std::uint64_t key, std::uint64_t v) {
    return key ^ (v + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2));
}

struct ObjectState {
    bool ReceiveShadows = true;
    bool StaticReceiver = false;
    bool NoSsao = false;
    bool DoubleSided = false;
    bool Prepassed = false;
    std::uint32_t LayerBit = 0;
    std::uint32_t ProbeKey = 0;  // ReflectionProbeArray::SelectionKey (all 32 bits)
    std::uint64_t GIMask = 0;    // GIVolumes::MaskFor
};

// Equal for two groups only when the per-draw hook would bind them alike.
inline std::uint64_t Object(const ObjectState& s) {
    std::uint64_t k = (std::uint64_t)s.ReceiveShadows | ((std::uint64_t)s.StaticReceiver << 1) | ((std::uint64_t)s.NoSsao << 2) |
                      ((std::uint64_t)s.DoubleSided << 3) | ((std::uint64_t)s.Prepassed << 4);
    k = Mix(k, s.LayerBit);
    k = Mix(k, s.ProbeKey);
    k = Mix(k, s.GIMask);
    return k;
}

} // namespace OpaqueBatchKey
