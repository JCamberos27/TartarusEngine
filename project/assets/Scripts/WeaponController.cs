using Tartarus;
using System.Numerics;

namespace Tartarus.Gameplay;

// Shared by AK and 870; their definitions and animator contracts supply all differences.
public sealed class WeaponController
{
    public const int Hidden = 1, Reloading = 2, Busy = 4, Cycling = 8, Ads = 16, Idle = 32, Ready = 64, IkOff = 128;
    
    public const int Dry = 1, Commit = 2, FireTrigger = 4, EndBurst = 8, Cycle = 16, Fidget = 32, ReloadTrigger = 64, MagCheck = 128;
    
    static bool Tag(in WeaponFrame w, int tag) => (w.Tags & tag) != 0;
    
    static void ResetBurst(ref WeaponFrame w) { w.BurstRemaining = 0; w.Commands |= EndBurst; }
    
    public static Vector3 PelletDirection(Vector3 direction, float spread, float u1, float u2)
    {
        if (!(spread > 0)) return direction;
        Vector3 helper = MathF.Abs(direction.Y) < .9f ? Vector3.UnitY : Vector3.UnitX;
        Vector3 side = Vector3.Normalize(Vector3.Cross(direction, helper)), up = Vector3.Cross(side, direction);
        float radius = MathF.Tan(spread * MathF.PI / 180) * MathF.Sqrt(Math.Clamp(u1, 0, 1));
        float angle = 2 * MathF.PI * u2;
        return Vector3.Normalize(direction + side * (radius * MathF.Cos(angle)) + up * (radius * MathF.Sin(angle)));
    }
    
    public void FireShot(ref ShotFrame shot)
    {
        int pellets = Math.Max(1, shot.Pellets);
        var random = new Random(shot.RandomSeed);
        for (int p = 0; p < pellets; ++p)
        {
            Vector3 direction = pellets > 1 || shot.Spread > 0
                ? PelletDirection(shot.Direction, shot.Spread, random.NextSingle(), random.NextSingle()) : shot.Direction;
            bool struck = Engine.Raycast(shot.Origin, direction, shot.Range, out NativeRequest hit, record: false);
            NativeRequest trace = new() {
                A = shot.Origin, B = struck ? hit.A : shot.Origin + direction * shot.Range,
                C = struck ? hit.B : Vector3.Zero, Value = shot.BulletHoleRadius,
                Entity = struck ? hit.Entity : uint.MaxValue, Result = struck ? 1 : 0
            };
            Engine.Call(29, ref trace);
            if (!struck || shot.ImpactImpulse <= 0 || !Engine.DynamicBodyMass(hit.Entity, out float mass)) continue;
            float impulse = shot.ImpactImpulse;
            if (shot.ImpactMaxSpeed > 0) impulse = Math.Min(impulse, shot.ImpactMaxSpeed * Math.Max(mass, .01f));
            Engine.ApplyImpulse(hit.Entity, direction * (impulse / pellets), hit.A);
        }
    }
    
    public void Update(ref WeaponFrame w)
    {
        w.Commands = 0; w.Result = 0;
        switch (w.Operation)
        {
            case 1: // Fire request
                if (w.Equipped == 0 || Tag(w, Hidden)) return;
                if (w.PerRound != 0 && Tag(w, Reloading)) { w.StopReload = w.Ammo > 0 ? 1 : 0; return; }
                if (w.Ammo <= 0)
                {
                    if (!Tag(w, Reloading | Busy)) { w.Commands |= Dry; w.Cooldown = Math.Max(w.Cooldown, .33f); }
                    return;
                }
                if (w.Chambered == 0 || Tag(w, Cycling | Reloading | Busy) || w.WallBlocked != 0) return;
                bool procedural = Tag(w, Ads) || w.HipProcedural != 0 && Tag(w, Idle | Ready) || w.RecoilProfile != 0;
                if (procedural)
                {
                    if (Tag(w, IkOff) || !Tag(w, Ads | Idle | Ready)) return;
                    w.Commands |= Commit;
                }
                else { w.Commands |= FireTrigger; w.WaitingShot = 1; }
                w.Result = 1;
                break;
            case 2: // Animation or procedural shot committed
                w.Ammo = Math.Max(0, w.Ammo - 1); w.SinceShot = 0; w.IdleTime = 0;
                if (w.CycleAfterShot != 0) { w.Chambered = 0; w.CycleSeen = 0; w.CycleWait = w.CycleDelay; }
                break;
            case 3: // Trigger eligibility
                if (w.Pressed == 0 && w.Held == 0) ResetBurst(ref w);
                if (w.Cooldown > 0) return;
                if (w.FullAuto == 0 && w.Pressed != 0 && w.BurstRounds > 1) w.BurstRemaining = w.BurstRounds;
                w.Result = w.FullAuto != 0 ? w.Held : (w.Pressed != 0 || w.BurstRemaining > 0 ? 1 : 0);
                break;
            case 4: // Trigger result (native presentation attempted Fire)
                if (w.Pressed == 0) { ResetBurst(ref w); return; }
                w.Cooldown = 60 / Math.Max(1, w.Rpm);
                if (w.BurstRemaining > 0) --w.BurstRemaining;
                w.Result = (w.FullAuto != 0 || w.UnityRecoil != 0) && w.Held != 0 || w.BurstRemaining > 0 ? 1 : 0;
                break;
            case 5:
                if (w.Equipped == 0 || Tag(w, Reloading) || w.Ammo >= w.Magazine) return;
                w.StopReload = 0; ResetBurst(ref w); w.Commands |= ReloadTrigger; w.Result = 1;
                break;
            case 6:
                if (w.Equipped == 0 || w.AllowFullAuto == 0) return;
                w.FullAuto = w.FullAuto == 0 ? 1 : 0; ResetBurst(ref w); w.Result = 1;
                break;
            case 7:
                w.Cooldown = Math.Max(0, w.Cooldown - w.Dt);
                w.SinceShot += w.Dt;
                w.SinceUnhidden = Tag(w, Hidden) ? 0 : w.SinceUnhidden + w.Dt;
                if (w.Equipped == 0 || w.Ammo <= 0 || Tag(w, Reloading | Busy | Hidden | Cycling) || w.WallBlocked != 0) ResetBurst(ref w);
                if (w.PerRound != 0 && !Tag(w, Reloading)) w.StopReload = 0;
                if (w.CycleAfterShot != 0 && w.Chambered == 0)
                {
                    if (Tag(w, Cycling)) w.CycleSeen = 1;
                    else if (w.CycleSeen != 0) w.Chambered = 1;
                    else { w.CycleWait -= w.Dt; if (w.CycleWait <= 0 && w.Equipped != 0 && !Tag(w, Hidden)) w.Commands |= Cycle; }
                }
                if (w.Equipped != 0 && Tag(w, Idle) && w.InTransition == 0)
                {
                    w.IdleTime += w.Dt;
                    if (w.IdleTime >= w.RegripDelay)
                    {
                        w.Commands |= Fidget; w.IdleTime = 0;
                        float min = Math.Max(0, w.RegripMin), max = Math.Max(min, w.RegripMax);
                        w.RegripDelay = min + (max - min) * Math.Clamp(w.Random, 0, 1);
                    }
                }
                else w.IdleTime = 0;
                break;
            case 8:
                if ((w.Events & 1) != 0) w.Ammo = w.Magazine;
                if ((w.Events & 2) != 0) { if (w.Ammo == 0) w.Chambered = 1; w.Ammo = Math.Min(w.Magazine, w.Ammo + 1); }
                break;
            case 9:
                if (w.Held != 0)
                {
                    if (w.ReloadWasDown == 0) { w.ReloadHeldSeconds = 0; w.ReloadHoldFired = 0; w.ReloadWasDown = 1; return; }
                    w.ReloadHeldSeconds += Math.Max(0, w.Dt);
                    if (w.ReloadHoldFired == 0 && w.ReloadHeldSeconds >= w.HoldSeconds) { w.ReloadHoldFired = 1; w.Commands |= MagCheck; }
                }
                else if (w.ReloadWasDown != 0) { if (w.ReloadHoldFired == 0) w.Result = 1; w.ReloadHeldSeconds = 0; w.ReloadHoldFired = 0; }
                w.ReloadWasDown = w.Held;
                break;
        }
    }
}
