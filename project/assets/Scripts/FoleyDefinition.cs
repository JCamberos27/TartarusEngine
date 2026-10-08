using Tartarus;
namespace Tartarus.Gameplay;
/// <summary>Project-owned sound/effect configuration.</summary>
public sealed class FoleyDefinition : Script
{
    public bool Enabled=true;
    public float Volume=1.0f;
    public float WalkVolume=1.0f;
    public float RunVolume=1.0f;
    public float CrouchVolume=0.55f;
    public float VolumeJitterDb=1.0f;
    public float PitchMin=0.94f;
    public float PitchMax=1.06f;
    public float StepStrideScale=1.0f;
    public float CrouchStrideScale=0.8f;
    public float MinStepSpeed=0.6f;
    public float RunSpeed=4.5f;
    public bool StepsFromFeet=true;
    public float FootLiftHeight=0.05f;
    public float FootLiftMoving=0.025f;
    public float FootContactHeight=0.02f;
    public float JumpVolume=1.0f;
    public float LandVolume=1.0f;
    public float LandMinSpeed=2.5f;
    public float LandFullSpeed=9.0f;
    public float LandHeavySpeed=6.0f;
    public float NpcStepVolume=1.0f;
    public float NpcStepMaxDistance=28.0f;
    public float NpcStepMinDistance=2.5f;
    public string DefaultSurface="concrete";
    public string SurfaceTable="wood=wood,plank,floor,parquet;metal=metal,steel,iron,grate;glass=glass;carpet=carpet,rug;water=water,puddle;concrete=concrete,asphalt,tile,stone,gravel,dirt,soil,sand,grass";
}
