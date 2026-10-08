using System.Numerics;
using Tartarus;

namespace Tartarus.Gameplay;

/// <summary>Project-owned player movement, view, loadout, ability and health configuration.</summary>
public sealed class PlayerDefinition : Script
{
    [AssetPath(".prefab")] public AssetReference PrimaryWeaponPrefab=new();
    [AssetPath(".prefab")] public AssetReference SecondaryWeaponPrefab=new();
    [Range(0.1f,50)] public float MoveSpeed=6.0f;
    [Range(0.1f,10)] public float SprintMultiplier=1.6f;
    [Range(0,30)] public float JumpSpeed=5.5f;
    [Range(0,1)] public float JumpBufferTime=0.12f;
    [Range(0,1)] public float CoyoteTime=0.10f;
    [Range(0,10)] public float GroundAccelTime=0.0f;
    [Range(0,10)] public float GroundDecelTime=0.0f;
    [Range(0,10)] public float AirAccelTime=0.0f;
    [Range(0.1f,4)] public float EyeHeight=1.6f;
    [Range(0.05f,2)] public float CapsuleRadius=0.3f;
    [Range(0.1f,6)] public float CapsuleHeight=1.8f;
    [Range(0.001f,5)] public float MouseSensitivity=0.1f;
    public bool InvertY=false;
    [Range(1,179)] public float FieldOfView=90.0f;
    [Range(0,2000)] public float StickLookDegPerSec=180.0f;
    [Range(0.001f,2)] public float EyeRadius=0.12f;
    public float KillY=-20.0f;
    public bool GravityGun=true;
    [Range(0,100)] public float Gravity=18.0f;
    [Range(0,100)] public float MinThrowSpeed=4.0f;
    [Range(0,100)] public float MaxThrowSpeed=18.0f;
    [Range(0.01f,10)] public float ThrowChargeTime=1.0f;
    [Range(0,20)] public float ThrowBackspin=2.0f;
    [Range(0,1000)] public float GrabRange=100.0f;
    [Range(0,1000)] public float AssistRange=30.0f;
    [Range(0,45)] public float AssistConeDeg=7.0f;
    [Range(0,180)] public float ScrollTurnDeg=15.0f;
    [AssetPath(".fpsanim")] public AssetReference AnimationSet=new();
    [AssetPath(".fpsanim")] public AssetReference SecondaryAnimationSet=new();
    public string CameraBone="head";
    public Vector3 ViewModelOffset=Vector3.Zero;
    public Vector3 ViewModelRotation=Vector3.Zero;
    [Range(0.01f,10)] public float ViewModelScale=1.0f;
    [Range(20,150)] public float ViewModelFov=60.0f;
    [Range(1,100000)] public float MaxHealth=100.0f;
    [Range(0,120)] public float RegenDelay=5.0f;
    [Range(0,10000)] public float RegenRate=30.0f;
    [Range(0,60)] public float RespawnDelay=3.0f;
    [Range(0,60)] public float SpawnProtection=2.0f;
}
