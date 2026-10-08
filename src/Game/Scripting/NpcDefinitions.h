#pragma once
#include <entt/entt.hpp>
class World;
struct SquadSettingsComponent;
struct ImpactAudioComponent;
struct WeaponAudioComponent;
struct FoleyAudioComponent;
struct FxHudSettingsComponent;
namespace Scripting {
void SyncNpcDefinitions(World& world);
void SyncNpcDefinition(World& world,entt::entity entity);
SquadSettingsComponent DefaultSquadDefinition();
ImpactAudioComponent DefaultImpactAudioDefinition();
WeaponAudioComponent DefaultWeaponAudioDefinition();
FoleyAudioComponent DefaultFoleyDefinition();
FxHudSettingsComponent DefaultEffectsDefinition();
}
