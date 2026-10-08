using Tartarus;
namespace Tartarus.Gameplay;
public sealed class ImpactAudioDefinition : Script
{
    public bool Enabled=true;
    public string SurfaceTable="metal=metal,steel,iron,grate;wood=wood,plank,floor,parquet;tile=tile;carpet=carpet,rug;glass=glass;ice=ice,snow;dirt=dirt,soil,sand,grass,gravel,mud;concrete=concrete,asphalt,stone,brick";
    public string DefaultSurface="concrete";
    public bool CasingsEnabled=true;
    public int CasingMaxContacts=2;
    public float CasingMinSpeed=0.9f;
    public float CasingFullSpeed=4.0f;
    public float CasingGainMin=0.25f;
    public float CasingVolume=1.0f;
    public float CasingMinDistance=1.5f;
    public float CasingMaxDistance=25.0f;
    public int CasingMaxVoices=6;
    public float ShellRadius=0.0075f;
    public bool ImpactsEnabled=true;
    public float ImpactVolume=1.0f;
    public float ImpactMinDistance=2.0f;
    public float ImpactMaxDistance=60.0f;
    public int ImpactMaxVoices=8;
    public float ImpactMinInterval=0.03f;
    public bool FleshUsesRecordings=true;
    public float FleshVolume=1.0f;
    public bool FlybyEnabled=true;
    public float FlybyRadius=5.0f;
    public float FlybyVolume=1.0f;
    public float FlybyMinInterval=0.07f;
    public float FlybyMinDistance=0.5f;
    public float FlybyMaxDistance=14.0f;
    public float FlybyFarGain=0.35f;
}
