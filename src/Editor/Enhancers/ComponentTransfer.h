#pragma once
#include "Components.h" // TransformComponent (kept by value)

#include <entt/entt.hpp>

#include <string>
#include <vector>

class World;
class AssetLibrary;

// Editor Enhancers / vInspector - moving component values between objects and across Play/Stop.
// Built on SceneSerializer's preset JSON (ComponentToPresetJson / ApplyComponentPresetJson), so
// only generically-serialised components take part; Transform has its own by-value path for
// "keep play-mode changes". Undo is the caller's: push once before calling anything that writes.
namespace Enhancers {

// --- Multi-component clipboard ---------------------------------------------------------------
enum class PasteMode {
    AsNew,  // add only the components the target lacks
    Values, // overwrite only the components the target already has
};

// The preset JSON of every generically-serialised component on `e` (registration order).
std::vector<std::string> CopyAllComponents(const World& world, entt::entity e);
// Whether `presetJson` would be written onto `e` under `mode`.
bool CanPastePreset(const World& world, entt::entity e, const std::string& presetJson, PasteMode mode);
// How many clipboard entries would apply to `e`.
int CountPastable(const World& world, entt::entity e, const std::vector<std::string>& clip, PasteMode mode);

struct PasteReport {
    int Applied = 0;
    std::vector<std::string> Skipped; // component names that didn't fit `mode` (or failed)
};
PasteReport PasteComponents(World& world, AssetLibrary& assets, entt::entity e,
                            const std::vector<std::string>& clip, PasteMode mode);

// --- Keep play-mode changes ---------------------------------------------------------------------
inline constexpr const char* kKeepTransform = "Transform";

struct PlayKeep {
    int Order = -1;
    std::string Component; // ComponentRegistry name, or kKeepTransform
    bool operator==(const PlayKeep& o) const { return Order == o.Order && Component == o.Component; }
};

struct KeptValue {
    int Order = -1;
    std::string Component;
    std::string Preset;            // registry components
    TransformComponent Transform;  // kKeepTransform
};

// Reads each requested component off the Play world, before Stop reloads the snapshot. Requests
// whose object or component is gone are dropped.
std::vector<KeptValue> CaptureKept(const World& playWorld, const std::vector<PlayKeep>& keeps);
// Writes captured values onto the restored world by OrderComponent value. A component removed
// during Play but present before is left alone; one added during Play is added. Returns the
// number of values written.
int ApplyKept(World& world, AssetLibrary& assets, const std::vector<KeptValue>& kept);

} // namespace Enhancers
