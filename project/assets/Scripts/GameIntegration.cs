using System.Runtime.CompilerServices;

namespace Tartarus.Gameplay;

// The native adapter supplies state/services; all operation meanings belong to this project.
internal static unsafe class GameIntegration
{
    public static bool Invoke(string operation,nint data,int size)
    {
        if(data==0)return false;
        switch(operation)
        {
            case "vitals" when size==Unsafe.SizeOf<VitalsFrame>(): VitalsRules.Update(ref *(VitalsFrame*)data);return true;
            case "damage" when size==Unsafe.SizeOf<DamageFrame>(): DamageRules.Update(ref *(DamageFrame*)data);return true;
            case "gravity" when size==Unsafe.SizeOf<GravityFrame>(): GravityAbility.Update(ref *(GravityFrame*)data);return true;
            case "ai.rules" when size==Unsafe.SizeOf<AiRuleFrame>(): AiRules.Update(ref *(AiRuleFrame*)data);return true;
            case "ai.choose" when size==Unsafe.SizeOf<AiChoiceFrame>(): NpcDecisions.Choose(ref *(AiChoiceFrame*)data);return true;
            case "npc.damage" when size==Unsafe.SizeOf<NpcDamageFrame>(): NpcDamageRules.Update(ref *(NpcDamageFrame*)data);return true;
            case "npc.behaviour" when size==Unsafe.SizeOf<NpcBrainFrame>(): NpcBehaviour.Update((NpcBrainFrame*)data);return true;
            case "hud" when size==Unsafe.SizeOf<CombatHudFrame>():CombatHud.Update(ref *(CombatHudFrame*)data);return true;
            case "npc.perception" when size==Unsafe.SizeOf<NpcPerceptionFrame>():NpcPerception.Update(ref *(NpcPerceptionFrame*)data);return true;
            case "npc.squads" when size==Unsafe.SizeOf<NpcSquadFrame>():NpcSquads.Update(ref *(NpcSquadFrame*)data);return true;
            case "npc.combat" when size==Unsafe.SizeOf<NpcCombatFrame>():NpcCombat.Update(ref *(NpcCombatFrame*)data);return true;
            case "audio.shot" when size==Unsafe.SizeOf<AudioShotFrame>():AudioPolicy.Shot(ref *(AudioShotFrame*)data);return true;
            case "audio.foley" when size==Unsafe.SizeOf<FoleyFrame>():FoleyPolicy.Update(ref *(FoleyFrame*)data);return true;
            case "body.motion" when size==Unsafe.SizeOf<BodyMotionFrame>():BodyLocomotion.Update(ref *(BodyMotionFrame*)data);return true;
            case "npc.callouts" when size==Unsafe.SizeOf<NpcCalloutsFrame>():NpcCallouts.Update(ref *(NpcCalloutsFrame*)data);return true;
            case "wardrobe.random" when size==Unsafe.SizeOf<WardrobeRandomFrame>():WardrobePolicy.Randomize(ref *(WardrobeRandomFrame*)data);return true;
            case "npc.spawn" when size==Unsafe.SizeOf<NpcSpawnFrame>():NpcLifecycle.Spawn(ref *(NpcSpawnFrame*)data);return true;
            case "npc.life" when size==Unsafe.SizeOf<NpcLifeFrame>():NpcLifecycle.Life(ref *(NpcLifeFrame*)data);return true;
            case "npc.player-noise" when size==Unsafe.SizeOf<NpcPlayerNoiseFrame>():NpcLifecycle.Noise(ref *(NpcPlayerNoiseFrame*)data);return true;
            case "npc.respawn" when size==Unsafe.SizeOf<NpcRespawnFrame>():NpcLifecycle.Respawn(ref *(NpcRespawnFrame*)data);return true;
            case "npc.shot-pose" when size==Unsafe.SizeOf<NpcShotPoseFrame>():NpcLifecycle.Pose(ref *(NpcShotPoseFrame*)data);return true;
            case "dev" when size==Unsafe.SizeOf<DevFrame>():DevTools.Update(ref *(DevFrame*)data);return true;
            case "session" when size==Unsafe.SizeOf<GameSessionFrame>():GameSession.Update(ref *(GameSessionFrame*)data);return true;
            case "npc.shot-effects" when size==Unsafe.SizeOf<NpcShotEffectFrame>():NpcShotEffects.Update(ref *(NpcShotEffectFrame*)data);return true;
            case "audio.impact" when size==Unsafe.SizeOf<ImpactFrame>():ImpactPolicy.Update(ref *(ImpactFrame*)data);return true;
            case "weapon.loadout" when size==Unsafe.SizeOf<WeaponLoadoutFrame>():WeaponLoadout.Update(ref *(WeaponLoadoutFrame*)data);return true;
            default:return false;
        }
    }
    public static string Request(string operation,string json) => operation switch
    {
        "audio.gun" or "audio.folder" or "audio.prefix" or "audio.parse" or "audio.default-profile" or "audio.profiles" or "audio.environment-profile" => AudioPolicy.Request(operation,json),
        "wardrobe.name" or "wardrobe.hat-name" or "wardrobe.layer" or "wardrobe.hides" or "wardrobe.gender" => WardrobePolicy.Request(operation,json),
        "audio.foley-element" or "audio.foley-candidates" => FoleyPolicy.Request(operation,json),
        "audio.cue" or "audio.impact-set" or "audio.impact-surface" => ImpactPolicy.Request(operation,json),
        "audio.surface" => FoleyPolicy.Surface(json),
        "body.roles" or "body.graph" or "body.pick" => BodyGraphAuthoring.Request(operation,json),
        "bone-region" => System.Text.Json.JsonSerializer.Serialize(DamageRules.BoneRegion(System.Text.Json.JsonSerializer.Deserialize<string>(json))),
        "weapon.import" or "weapon.resolve" => WeaponData.Request(operation,json),
        "health.create" or "health.remove" or "health.kill" or "health.read" => HealthRules.Request(operation,json),
        "npc.name" or "npc.spawn-plan" => NpcLifecycle.Request(operation,json),
        "content.path" or "content.blood" or "content.knife" => ContentTools.Request(operation,json),
        "npc.config" => NpcConfigData.Resolve(json),
        "basketball.state" => System.Text.Json.JsonSerializer.Serialize(new {home=Scoreboard.Boards.FirstOrDefault()?.Home??-1,away=Scoreboard.Boards.FirstOrDefault()?.Away??-1}),
        _ => throw new ArgumentException("Unknown project request: "+operation)
    };
}
