// Dropping an Asset Browser item onto an object (the Scene viewport or a Hierarchy row), like
// Unity: a material onto the part under the cursor, a texture as that part's albedo, a sound as
// the object's Audio Source, an Animator Controller / weapon definition / script onto the matching
// component, an .hdr anywhere as the sky. Models and prefabs are placed by the drop targets
// themselves (DrawViewportDropTarget / InstantiateAssetDropInHierarchy).
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "AssetLibrary.h"
#include "MaterialAsset.h"
#include "Model.h"
#include "World.h"
#include "Components.h"
#include "ProjectPaths.h"
#include "Log.h"

#include <filesystem>

using namespace EditorInternal;

namespace {

std::string LowerExt(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

std::string FileName(const std::string& path) { return std::filesystem::path(path).filename().string(); }

std::string EntityName(const World& world, entt::entity e) {
    if (const auto* n = world.Registry.try_get<NameComponent>(e); n && !n->Name.empty()) return n->Name;
    return "the object";
}

// The renderer a material / texture drop acts on, and the slot: the one under the cursor, else 0.
RenderableComponent* DropRenderable(World& world, entt::entity e, int& slot) {
    if (e == entt::null || !world.Registry.valid(e)) return nullptr;
    auto* rc = world.Registry.try_get<RenderableComponent>(e);
    if (!rc || !rc->ModelRef || rc->ModelRef->MeshCount() == 0) return nullptr;
    if (slot < 0 || slot >= rc->ModelRef->MeshCount()) slot = 0;
    return rc;
}

std::string SlotLabel(const RenderableComponent& rc, int slot) {
    if (rc.ModelRef->MeshCount() <= 1) return {};
    const std::string& part = rc.ModelRef->MeshMaterial(slot).Name;
    return " (slot " + std::to_string(slot) + (part.empty() ? "" : ": " + part) + ")";
}

bool IsTransformControllerScript(const std::string& path) {
    return std::filesystem::path(path).stem().string() == "TransformController";
}

} // namespace

std::string EditorLayer::AssetDropHint(World& world, entt::entity target, const std::string& payloadType,
                                       const std::string& path, int slot) const {
    const std::string ext = LowerExt(path);
    const std::string file = FileName(path);
    const bool onObject = target != entt::null && world.Registry.valid(target);
    const std::string name = onObject ? EntityName(world, target) : std::string();

    if (payloadType == "ASSET_TEXTURE_PATH" && ext == ".hdr") return "Use " + file + " as the sky (HDRI)";
    if (payloadType == "ASSET_TEXTURE_PATH" && ext == ".exr") return ".exr skies aren't supported - convert it to .hdr";
    if (payloadType == "ASSET_MATERIAL_PATH" || payloadType == "ASSET_TEXTURE_PATH") {
        const RenderableComponent* rc = DropRenderable(world, target, slot);
        if (!rc) return {};
        return (payloadType == "ASSET_MATERIAL_PATH" ? "Apply " + file + " to " : "Set " + file + " as the albedo of ") +
               name + SlotLabel(*rc, slot);
    }
    if (payloadType == "ASSET_SOUND_PATH") {
        if (!onObject) return "Create an Audio Source playing " + file + " here";
        return world.Registry.all_of<AudioSourceComponent>(target) ? "Set " + name + "'s Audio Source to " + file
                                                                    : "Add an Audio Source playing " + file + " to " + name;
    }
    if (payloadType == "ASSET_FILE_PATH" && onObject) {
        if (ext == ".controller") return "Animate " + name + " with " + file;
        if (ext == ".fpsanim")
            return world.Registry.all_of<FirstPersonControllerComponent>(target) ? "Use " + file + " as " + name + "'s weapon"
                                                                                : std::string();
        if (ext == ".tescript" && IsTransformControllerScript(path)) return "Add a Transform Controller to " + name;
    }
    return {};
}

bool EditorLayer::ApplyAssetDrop(World& world, AssetLibrary& assets, entt::entity target, const std::string& payloadType,
                                 const std::string& path, int slot, const glm::vec3& dropPosition) {
    const std::string ext = LowerExt(path);
    const std::string rel = ProjectPaths::Relativize(path);
    const bool onObject = target != entt::null && world.Registry.valid(target);

    if (payloadType == "ASSET_TEXTURE_PATH" && (ext == ".hdr" || ext == ".exr")) {
        if (ext == ".exr") { Log::Warn("HDRI sky: .exr isn't supported - convert '" + FileName(path) + "' to .hdr."); return false; }
        PushUndo(world, "Set HDRI Sky");
        world.SkyHdriPath = path;
        world.SkySourceMode = World::SkySource::Hdri;
        return true;
    }

    if (payloadType == "ASSET_MATERIAL_PATH") {
        RenderableComponent* rc = DropRenderable(world, target, slot);
        if (!rc) return false;
        auto mat = assets.LoadMaterial(path);
        if (!mat || mat->Missing) { Log::Error("Couldn't load material '" + rel + "'."); return false; }
        PushUndo(world, "Assign Material");
        if ((int)rc->Materials.size() <= slot) rc->Materials.resize(slot + 1);
        rc->Materials[slot] = mat;
        return true;
    }

    if (payloadType == "ASSET_TEXTURE_PATH") {
        RenderableComponent* rc = DropRenderable(world, target, slot);
        if (!rc) return false;
        auto tex = assets.LoadTexture(path);
        if (!tex) return false;
        PushUndo(world, "Set Albedo Map");
        if ((int)rc->Materials.size() <= slot) rc->Materials.resize(slot + 1);
        auto& m = rc->Materials[slot];
        if (!m) {
            // No material in this slot yet: start from the imported one's other maps.
            m = std::make_shared<MaterialAsset>();
            const Material& imported = rc->ModelRef->MeshMaterial(slot);
            m->Mat.NormalMap    = imported.NormalMap;
            m->Mat.MetallicMap  = imported.MetallicMap;
            m->Mat.RoughnessMap = imported.RoughnessMap;
            m->Mat.AOMap        = imported.AOMap;
            m->Mat.EmissiveMap  = imported.EmissiveMap;
        } else if (!m->Path.empty()) {
            // A .mat file is shared by everything using it: give this object its own copy rather
            // than repaint every other one (the file itself is left alone).
            m = std::make_shared<MaterialAsset>(*m);
            m->Path.clear();
        }
        m->Mat.AlbedoMap = tex;
        return true;
    }

    if (payloadType == "ASSET_SOUND_PATH") {
        if (onObject) {
            PushUndo(world, "Set Audio Source");
            world.Registry.get_or_emplace<AudioSourceComponent>(target).SoundPath = path;
            return true;
        }
        PushUndo(world, "Create Audio Source");
        const entt::entity e = world.CreateEmptyEntity(dropPosition, glm::vec3(0.0f), glm::vec3(1.0f),
                                                       UniqueNameFor(world, std::filesystem::path(path).stem().string()));
        world.Registry.emplace_or_replace<AudioSourceComponent>(e).SoundPath = path;
        SelectItem(e, false);
        return true;
    }

    if (payloadType == "ASSET_FILE_PATH" && onObject) {
        if (ext == ".controller") {
            PushUndo(world, "Set Animator Controller");
            world.Registry.get_or_emplace<AnimatorControllerComponent>(target).Controller = rel;
            return true;
        }
        if (ext == ".fpsanim") {
            auto* fpc = world.Registry.try_get<FirstPersonControllerComponent>(target);
            if (!fpc) {
                Log::Warn("'" + FileName(path) + "' goes on an object with a First Person Controller.");
                return false;
            }
            PushUndo(world, "Set Weapon Definition");
            fpc->AnimationSet = rel;
            return true;
        }
        if (ext == ".tescript") {
            if (!IsTransformControllerScript(path)) {
                Log::Warn("'" + FileName(path) + "' isn't a script the editor knows how to attach.");
                return false;
            }
            PushUndo(world, "Add Transform Controller");
            world.Registry.get_or_emplace<TransformControllerComponent>(target).ScriptPath = rel;
            return true;
        }
    }
    return false;
}
