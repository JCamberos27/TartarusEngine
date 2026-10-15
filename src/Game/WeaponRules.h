#pragma once
#include "GameFrames.h"

// Weapon gameplay rules shared by every first-person gun (AK, 870, ...). The definitions and
// animator contracts supply the differences; these functions only read and write the frame.
namespace WeaponRules {
// Animator tags packed into WeaponFrame::Tags.
enum Tag : int { Hidden = 1, Reloading = 2, Busy = 4, Cycling = 8, Ads = 16, Idle = 32, Ready = 64, IkOff = 128 };
// Requests written to WeaponFrame::Commands for the presentation to carry out.
enum Command : int { Dry = 1, Commit = 2, FireTrigger = 4, EndBurst = 8, Cycle = 16, Fidget = 32, ReloadTrigger = 64, MagCheck = 128 };

// Operations: 1 fire request, 2 shot committed, 3 trigger eligibility, 4 trigger result, 5 reload,
// 6 fire-mode toggle, 7 tick, 8 animation events (1 magazine, 2 shell), 9 reload key hold/tap.
void Update(Gameplay::WeaponFrame& w);

// Operations: 0 select slot, 1 cycle slots, 2 equip/holster, 3 refill, 4 swap when hidden.
// Commands: 1 swap now, 2 end burst, 4 sync animator, 8 gear sound, 16 already selected, 32 select Value.
void UpdateLoadout(Gameplay::WeaponLoadoutFrame& f);
}
