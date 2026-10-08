#pragma once
#include <array>
#include <string>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
class World;
namespace Scripting {
enum class AttachmentKind { Muzzle, Grip, Optic };
struct WeaponAttachment {
    entt::entity Entity = entt::null;
    entt::entity Point = entt::null;
    entt::entity Flash = entt::null;
    std::string FiringSounds;
    std::string FiringSoundProfile;
    float AimBlendTime = .2f;
    bool DefaultOptic = false;
};
// Reads the public fields of the prefab's Attachment scripts. References are relative hierarchy paths.
class WeaponAttachments {
public:
    void Load(World& world, entt::entity root, std::array<int, 3> preferred = {{-1, -1, -1}});
    bool Cycle(World& world, AttachmentKind kind);
    const WeaponAttachment* Selected(AttachmentKind kind) const;
    bool Pose(const World& world, entt::entity root, AttachmentKind kind, glm::mat4& out) const;
    bool ReferenceOpticPose(const World& world, entt::entity root, glm::mat4& out) const;
    bool OwnsEmitter(const World& world, entt::entity emitter) const;
    int Count(AttachmentKind kind) const { return (int)m_Items[(int)kind].size(); }
    const std::array<int, 3>& Indices() const { return m_Selected; }
private:
    std::array<std::vector<WeaponAttachment>, 3> m_Items;
    std::array<int, 3> m_Selected{{-1, -1, -1}};
};
}
