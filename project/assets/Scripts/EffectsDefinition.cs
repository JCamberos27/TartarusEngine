using Tartarus;
namespace Tartarus.Gameplay;
/// <summary>Project-owned sound/effect configuration.</summary>
public sealed class EffectsDefinition : Script
{
    public float FlashTime=0.055f;
    public float PlayerFlashScale=0.35f;
    public float FlameGlow=150.0f;
    public float FlameScale=1.75f;
    public int MuzzleStyle=1;
    public float BeamRange=150.0f;
    public float BeamHalfWidth=0.0015f;
    public float BeamFalloff=2.5f;
    public float BeamBend=4.0f;
    public float FeedLife=4.5f;
    public float StreakWindow=4.0f;
}
