using Tartarus;
namespace Tartarus.Gameplay;
/// <summary>Project-owned sample NPC configuration.</summary>
public enum NpcLoadout {Rifle,Shotgun,Random}
public enum NpcBrainKind {Squad,TrainingDummy}
public sealed class NpcDefinition : Script
{
    public NpcLoadout Weapon=NpcLoadout.Rifle;
    public int Squad=0;
    [Range(0,1)] public float Skill=0.5f;
    public int OutfitSeed=0;
    public NpcBrainKind Brain=NpcBrainKind.Squad;
}
