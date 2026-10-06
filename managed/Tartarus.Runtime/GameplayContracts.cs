namespace Tartarus;

/// <summary>Requests sent by the native presentation adapter to IGameplay. Gameplay decides results and commands.</summary>
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
[Flags] public enum WeaponEvents { None = 0, MagazineLoaded = 1, ShellLoaded = 2 }
public static class GameplayFrameExtensions
{
    public static bool HasTag(this in WeaponFrame frame, WeaponTags tag) => (frame.Tags & (int)tag) != 0;
    public static bool HasCommand(this in WeaponFrame frame, WeaponCommands command) => (frame.Commands & (int)command) != 0;
}
