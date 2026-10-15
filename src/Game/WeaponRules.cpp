#include "WeaponRules.h"

#include <algorithm>

namespace WeaponRules {
namespace {
bool Has(const Gameplay::WeaponFrame& w, int tag) { return (w.Tags & tag) != 0; }
void ResetBurst(Gameplay::WeaponFrame& w) { w.BurstRemaining = 0; w.Commands |= EndBurst; }
}

void Update(Gameplay::WeaponFrame& w) {
    w.Commands = 0; w.Result = 0;
    switch (w.Operation) {
    case 1: { // Fire request
        if (!w.Equipped || Has(w, Hidden)) return;
        if (w.PerRound && Has(w, Reloading)) { w.StopReload = w.Ammo > 0 ? 1 : 0; return; }
        if (w.Ammo <= 0) {
            if (!Has(w, Reloading | Busy)) { w.Commands |= Dry; w.Cooldown = std::max(w.Cooldown, .33f); }
            return;
        }
        if (!w.Chambered || Has(w, Cycling | Reloading | Busy) || w.WallBlocked) return;
        const bool procedural = Has(w, Ads) || (w.HipProcedural && Has(w, Idle | Ready)) || w.RecoilProfile;
        if (procedural) {
            if (Has(w, IkOff) || !Has(w, Ads | Idle | Ready)) return;
            w.Commands |= Commit;
        } else { w.Commands |= FireTrigger; w.WaitingShot = 1; }
        w.Result = 1;
        break;
    }
    case 2: // Animation or procedural shot committed
        w.Ammo = std::max(0, w.Ammo - 1); w.SinceShot = 0; w.IdleTime = 0;
        if (w.CycleAfterShot) { w.Chambered = 0; w.CycleSeen = 0; w.CycleWait = w.CycleDelay; }
        break;
    case 3: // Trigger eligibility
        if (!w.Pressed && !w.Held) ResetBurst(w);
        if (w.Cooldown > 0) return;
        if (!w.FullAuto && w.Pressed && w.BurstRounds > 1) w.BurstRemaining = w.BurstRounds;
        w.Result = w.FullAuto ? w.Held : (w.Pressed || w.BurstRemaining > 0 ? 1 : 0);
        break;
    case 4: // Trigger result (Pressed carries whether Fire went off)
        if (!w.Pressed) { ResetBurst(w); return; }
        w.Cooldown = 60.0f / std::max(1.0f, w.Rpm);
        if (w.BurstRemaining > 0) --w.BurstRemaining;
        w.Result = ((w.FullAuto || w.UnityRecoil) && w.Held) || w.BurstRemaining > 0 ? 1 : 0;
        break;
    case 5: // Reload
        if (!w.Equipped || Has(w, Reloading) || w.Ammo >= w.Magazine) return;
        w.StopReload = 0; ResetBurst(w); w.Commands |= ReloadTrigger; w.Result = 1;
        break;
    case 6: // Fire-mode toggle
        if (!w.Equipped || !w.AllowFullAuto) return;
        w.FullAuto = w.FullAuto ? 0 : 1; ResetBurst(w); w.Result = 1;
        break;
    case 7: // Tick
        w.Cooldown = std::max(0.0f, w.Cooldown - w.Dt);
        w.SinceShot += w.Dt;
        w.SinceUnhidden = Has(w, Hidden) ? 0 : w.SinceUnhidden + w.Dt;
        if (!w.Equipped || w.Ammo <= 0 || Has(w, Reloading | Busy | Hidden | Cycling) || w.WallBlocked) ResetBurst(w);
        if (w.PerRound && !Has(w, Reloading)) w.StopReload = 0;
        if (w.CycleAfterShot && !w.Chambered) {
            if (Has(w, Cycling)) w.CycleSeen = 1;
            else if (w.CycleSeen) w.Chambered = 1;
            else { w.CycleWait -= w.Dt; if (w.CycleWait <= 0 && w.Equipped && !Has(w, Hidden)) w.Commands |= Cycle; }
        }
        if (w.Equipped && Has(w, Idle) && !w.InTransition) {
            w.IdleTime += w.Dt;
            if (w.IdleTime >= w.RegripDelay) {
                w.Commands |= Fidget; w.IdleTime = 0;
                const float lo = std::max(0.0f, w.RegripMin), hi = std::max(lo, w.RegripMax);
                w.RegripDelay = lo + (hi - lo) * std::clamp(w.Random, 0.0f, 1.0f);
            }
        } else w.IdleTime = 0;
        break;
    case 8: // Animation events
        if (w.Events & 1) w.Ammo = w.Magazine;
        if (w.Events & 2) { if (w.Ammo == 0) w.Chambered = 1; w.Ammo = std::min(w.Magazine, w.Ammo + 1); }
        break;
    case 9: // Reload key: a tap reloads, a hold checks the magazine
        if (w.Held) {
            if (!w.ReloadWasDown) { w.ReloadHeldSeconds = 0; w.ReloadHoldFired = 0; w.ReloadWasDown = 1; return; }
            w.ReloadHeldSeconds += std::max(0.0f, w.Dt);
            if (!w.ReloadHoldFired && w.ReloadHeldSeconds >= w.HoldSeconds) { w.ReloadHoldFired = 1; w.Commands |= MagCheck; }
        } else if (w.ReloadWasDown) { if (!w.ReloadHoldFired) w.Result = 1; w.ReloadHeldSeconds = 0; w.ReloadHoldFired = 0; }
        w.ReloadWasDown = w.Held;
        break;
    }
}

void UpdateLoadout(Gameplay::WeaponLoadoutFrame& f) {
    f.Commands = 0;
    if (f.Operation == 3) {
        auto* ammo = reinterpret_cast<int*>(f.SlotAmmo);
        for (int i = 0; ammo && i < f.Count; ++i) ammo[i] = -1; // -1 = a full magazine when the slot is drawn
        if (!f.Active) return;
        f.Ammo = f.Magazine; f.Chambered = 1; f.CycleWait = 0; f.Commands = 4;
        return;
    }
    if (!f.Active) return;
    switch (f.Operation) {
    case 0:
        if (f.Value < 0 || f.Value >= f.Count) return;
        if (f.Value == f.Slot) { f.Pending = -1; f.Commands = 16; return; }
        f.Pending = f.Value;
        if (f.Equipped) f.Commands |= 8;
        f.Equipped = 0; f.Commands |= 4;
        break;
    case 1: {
        if (f.Count == 0 || f.Value == 0) return;
        const int at = f.Pending >= 0 ? f.Pending : f.Slot;
        f.Value = (at + (f.Value > 0 ? 1 : -1) + f.Count) % f.Count; f.Commands = 32;
        break;
    }
    case 2:
        if (!f.Requested) { f.Pending = -1; f.Burst = 0; f.Commands |= 2; }
        if (f.Requested != f.Equipped) f.Commands |= 8;
        f.Equipped = f.Requested; f.Commands |= 4;
        break;
    case 4:
        if (f.Pending >= 0 && f.Hidden && !f.InTransition) f.Commands = 1;
        break;
    }
}
}
