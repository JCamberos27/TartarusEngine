using Tartarus;
namespace Tartarus.Gameplay;
/// <summary>Project-owned sample NPC configuration.</summary>
public sealed class SquadDefinition : Script
{
    [Range(0,8)] public int SquadSize=4;
    public float RespawnDelay=8.0f;
    public float Difficulty=1.0f;
    public float NpcDamageScale=0.45f;
    public bool Respawn=true;
    public float HeavyHitDamage=40.0f;
    public float StaggerTime=0.4f;
    public float BleedOutTime=20.0f;
    public float CrawlSpeed=0.6f;
    public float LimpSpeedScale=0.6f;
    public float LimpTime=6.0f;
    public float CorpseTime=14.0f;
    public float FallGravity=18.0f;
    public float MeleeDamage=25.0f;
    public float MeleeTime=0.55f;
    public float MeleeHitTime=0.22f;
    public float HitboxRange=60.0f;
    public float FootIKRange=25.0f;
    public float MeshCheckRange=12.0f;
    public float CoverSpacing=0.9f;
    public float CoverReach=0.85f;
    public float CoverKneeHeight=0.85f;
    public float CoverHeadHeight=1.55f;
    public float CoverStep=0.8f;
    [AssetPath(".json")] public AssetReference BodyPrefab=new("assets/AI/Soldier.json");
    [AssetPath(".prefab")] public AssetReference RiflePrefab=new("assets/Weapons/AKS74U/AKS74U.prefab");
    [AssetPath(".prefab")] public AssetReference ShotgunPrefab=new("assets/Weapons/Remington870/Remington870.prefab");
    [AssetPath(".fpsanim")] public AssetReference RifleAnimationSet=new("assets/Weapons/AKS74U/AKS74U.fpsanim");
    [AssetPath(".fpsanim")] public AssetReference ShotgunAnimationSet=new("assets/Weapons/Remington870/Remington870.fpsanim");
}
