using System.Numerics;
using Tartarus;

namespace Tartarus.Gameplay;

/// <summary>Project-owned weapon prefab data. Native presentation only consumes its resolved settings.</summary>
public sealed class WeaponDefinition : Script
{
    [Header("Overview")]
    public string Description = "";
    [AssetPath(".fpsanim"), Tooltip("Animation, gameplay, recoil and camera profiles for this weapon.")]
    public AssetReference AnimationSet = new();
    public bool MuzzleEnabled = true;

    [Header("Gameplay"), Range(1,10000)] public int Magazine = 30;
    [Range(1,100000)] public float RoundsPerMinute = 700;
    public bool AllowFullAuto = true;
    [Range(0,32)] public int BurstRounds;
    [Range(.01f,10)] public float ReloadHoldSeconds = .35f;
    public float RegripMin = 10, RegripMax = 20;
    public float ImpactImpulse;
    public float ImpactMaxSpeed = 8;
    public float BulletHoleRadius = .004f;
    public float ZeroDistance = 25;
    public bool HasSightLine;
    public Vector3 SightOrigin;
    public Vector3 SightDirection = -Vector3.UnitZ;
    [Range(1,64)] public int Pellets = 1;
    [Range(0,45)] public float SpreadHip, SpreadAds;
    public enum ReloadStyle { Magazine, PerRound }
    public ReloadStyle Reload;
    public bool CycleAfterShot;
    [Range(0,2)] public float CycleDelay = .1f;

    [Header("Damage"), Range(0,10000)] public float Damage = 34;
    public float HeadMultiplier = 3, LimbMultiplier = .75f;
    public float FalloffStart = 25, FalloffEnd = 60;
    [Range(0,1)] public float FalloffMin = .6f;

    [Header("Muzzle Light"), Range(0.001f, 1), Tooltip("Light duration in seconds.")]
    public float FlashTime = 0.055f;
    [Range(0, 1000)] public float LightIntensity = 18;
    [Range(0, 2)] public float PlayerFlashScale = 0.35f;
    [Range(0, 100)] public float LightRange = 7;
    [Color] public Vector3 LightColor = new(1, 0.72f, 0.38f);

    [Header("Smoke")]
    public bool Smoke = true;
    public bool AfterfireSmoke = true;
    [Range(0, 10)] public float SmokeScale = 1;
    [Range(0.01f, 30)] public float SmokeLifetimeMin = 2;
    [Range(0.01f, 30)] public float SmokeLifetimeMax = 3;
    [Range(0, 1)] public float SmokeAlpha = 0.18f;

    // Preserve older prefab effects; child Particle Systems author the flash on current weapons.
    [HideInInspector] public int MuzzleStyle = 1;
    [HideInInspector, AssetPath(".png", ".jpg", ".tga")] public AssetReference FlameTexture = new("assets/Effects/Muzzle/T_MuzzleFlame.png");
    [HideInInspector] public float FlameGlow = 150;
    [HideInInspector] public float FlameScale = 1.75f;
    [HideInInspector] public Vector3 FlameColor = new(1, 0.147f, 0.0177f);
    [HideInInspector] public float FlameLifetimeMin = 0.125f;
    [HideInInspector] public float FlameLifetimeMax = 0.175f;
    [HideInInspector] public float FlameLengthMin = 0.16f;
    [HideInInspector] public float FlameLengthMax = 0.32f;
    [HideInInspector] public float FlameWidthMin = 0.04f;
    [HideInInspector] public float FlameWidthMax = 0.06f;
    [HideInInspector] public string FlashSprite = "muzzle_star";
    [HideInInspector] public float FlashLifetime = 0.065f;
    [HideInInspector] public float FlashSizeMin = 0.26f;
    [HideInInspector] public float FlashSizeMax = 0.34f;
    [HideInInspector] public float FlashIntensity = 14;
    [HideInInspector] public bool SideJets = true;
    [HideInInspector] public bool CoreGlow = true;
    [HideInInspector] public int SparkCount = 9;
}
