namespace Tartarus.Gameplay;

/// <summary>Requests sent by the native presentation adapter to the project gameplay integration. Gameplay decides results and commands.</summary>
public enum WeaponOperation
{
    RequestFire = 1, CommitShot, CheckTrigger, TriggerResult, RequestReload,
    ToggleFireMode, Tick, AnimationEvents, ReloadButton
}
[Flags] public enum WeaponTags
{
    None = 0, Hidden = 1, Reloading = 2, Busy = 4, Cycling = 8,
    Ads = 16, Idle = 32, Ready = 64, IkOff = 128
}
[Flags] public enum WeaponCommands
{
    None = 0, DryFire = 1, CommitShot = 2, FireTrigger = 4, EndBurst = 8,
    Cycle = 16, Fidget = 32, ReloadTrigger = 64, MagazineCheck = 128
}
/// <summary>GameSessionFrame.Commands bits (src/Game/Scripting/GameCommands.h holds the same numbers).</summary>
public static class SessionCommand
{
    public const int Slot1 = 1, Slot2 = 2, Unarmed = 4, CycleSlot = 8, Holster = 16, WeaponInput = 32, FireMode = 64,
        Inspect = 128, Melee = 256, ResetReload = 512, RefillAmmo = 1024, Died = 2048, Respawn = 4096, DeadCamera = 8192,
        Hitmarker = 16384, KillFeed = 32768, WorldImpact = 65536, Flashlight = 131072;
}
/// <summary>BodyMotionFrame.Triggers bits (src/Game/Scripting/GameCommands.h holds the same numbers).</summary>
public static class BodyMotionTrigger
{
    public const int CrouchDown = 1, CrouchUp = 2, Start = 4, Stop = 8, StopRun = 16, Jump = 32, Pivot = 64, Step = 128, Fidget = 256;
}
[Flags] public enum WeaponEvents { None = 0, MagazineLoaded = 1, ShellLoaded = 2 }
public static class GameplayFrameExtensions
{
    public static bool HasTag(this in WeaponFrame frame, WeaponTags tag) => (frame.Tags & (int)tag) != 0;
    public static bool HasCommand(this in WeaponFrame frame, WeaponCommands command) => (frame.Commands & (int)command) != 0;
}
