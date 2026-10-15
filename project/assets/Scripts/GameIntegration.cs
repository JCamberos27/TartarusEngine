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
            case "ai.rules" when size==Unsafe.SizeOf<AiRuleFrame>(): AiRules.Update(ref *(AiRuleFrame*)data);return true;
            case "ai.choose" when size==Unsafe.SizeOf<AiChoiceFrame>(): NpcDecisions.Choose(ref *(AiChoiceFrame*)data);return true;
            case "npc.behaviour" when size==Unsafe.SizeOf<NpcBrainFrame>(): NpcBehaviour.Update((NpcBrainFrame*)data);return true;
            case "npc.perception" when size==Unsafe.SizeOf<NpcPerceptionFrame>():NpcPerception.Update(ref *(NpcPerceptionFrame*)data);return true;
            case "npc.squads" when size==Unsafe.SizeOf<NpcSquadFrame>():NpcSquads.Update(ref *(NpcSquadFrame*)data);return true;
            case "npc.combat" when size==Unsafe.SizeOf<NpcCombatFrame>():NpcCombat.Update(ref *(NpcCombatFrame*)data);return true;
            case "body.motion" when size==Unsafe.SizeOf<BodyMotionFrame>():BodyLocomotion.Update(ref *(BodyMotionFrame*)data);return true;
            case "npc.callouts" when size==Unsafe.SizeOf<NpcCalloutsFrame>():NpcCallouts.Update(ref *(NpcCalloutsFrame*)data);return true;
            case "npc.spawn" when size==Unsafe.SizeOf<NpcSpawnFrame>():NpcLifecycle.Spawn(ref *(NpcSpawnFrame*)data);return true;
            case "npc.life" when size==Unsafe.SizeOf<NpcLifeFrame>():NpcLifecycle.Life(ref *(NpcLifeFrame*)data);return true;
            case "npc.player-noise" when size==Unsafe.SizeOf<NpcPlayerNoiseFrame>():NpcLifecycle.Noise(ref *(NpcPlayerNoiseFrame*)data);return true;
            case "npc.respawn" when size==Unsafe.SizeOf<NpcRespawnFrame>():NpcLifecycle.Respawn(ref *(NpcRespawnFrame*)data);return true;
            case "npc.shot-pose" when size==Unsafe.SizeOf<NpcShotPoseFrame>():NpcLifecycle.Pose(ref *(NpcShotPoseFrame*)data);return true;
            case "npc.shot-effects" when size==Unsafe.SizeOf<NpcShotEffectFrame>():NpcShotEffects.Update(ref *(NpcShotEffectFrame*)data);return true;
            case "weapon.loadout" when size==Unsafe.SizeOf<WeaponLoadoutFrame>():WeaponLoadout.Update(ref *(WeaponLoadoutFrame*)data);return true;
            default:return false;
        }
    }
    public static string Request(string operation,string json) => operation switch
    {
        "body.roles" or "body.graph" or "body.pick" => BodyGraphAuthoring.Request(operation,json),
        "npc.name" or "npc.spawn-plan" => NpcLifecycle.Request(operation,json),
        _ => throw new ArgumentException("Unknown project request: "+operation)
    };
}
