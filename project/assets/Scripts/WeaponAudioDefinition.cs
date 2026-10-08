using Tartarus;
namespace Tartarus.Gameplay;
/// <summary>Project-owned sound/effect configuration.</summary>
public sealed class WeaponAudioDefinition : Script
{
    public string Gun="ak";
    public bool Enabled=true;
    public float Volume=1.0f;
    public float PlayerGain=0.75f;
    public float ShotPitchMin=0.96f;
    public float ShotPitchMax=1.04f;
    public float VolumeJitterDb=0.5f;
    public float CloseFullDistance=18.0f;
    public float CloseZeroDistance=43.0f;
    public float FarMinWeight=0.1f;
    public float FarMaxWeight=1.0f;
    public float MaxDistance=90.0f;
    public float FarMaxDistance=160.0f;
    public float ShotMinDistance=3.0f;
    public float BassMinDistance=6.0f;
    public float EventMinDistance=1.5f;
    public float EventMaxDistance=25.0f;
    public int ShotMaxVoices=6;
    public int TailMaxVoices=3;
    public float TailFadeTime=0.25f;
    public float TailMinInterval=0.0f;
    public float TailDuckPerVoice=0.3f;
    public int TailEvery=2;
    public int FarEvery=3;
    public float BurstGap=0.4f;
    [AssetPath(".json")] public AssetReference DataFile=new("");
    public bool EnvEnabled=true;
    public int EnvRayCount=12;
    public float EnvMaxDistance=40.0f;
    public float EnvIndoorCover=0.7f;
    public float EnvUrbanWall=0.35f;
    public float EnvUrbanDistance=25.0f;
    public float EnvLargeRoomDistance=8.0f;
    public float EnvBlendFraction=0.15f;
    public float EnvBlendDistance=0.3f;
    public float EnvRefreshInterval=0.25f;
    public float EnvRefreshMoveDistance=1.0f;
    public float EnvMatchRadius=2.0f;
    public float EnvTailGainOutdoorOpen=1.0f;
    public float EnvTailGainOutdoorUrban=1.0f;
    public float EnvTailGainIndoorSmall=1.0f;
    public float EnvTailGainIndoorLarge=1.0f;
    public bool EnvDebugDraw=false;
}
