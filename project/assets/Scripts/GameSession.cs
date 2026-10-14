using System.Numerics;
using Tartarus;
namespace Tartarus.Gameplay;
internal static class GameSession
{
    internal static void Update(ref GameSessionFrame f){
        f.Commands=0;f.SlotDirection=0;
        switch(f.Operation){
        case 0:
            // Compute weapon ownership before equip changes, preserving the existing frame order.
            bool weaponInput=f.HasInput && !f.GravityLive && !f.Dead;f.AimHeld=weaponInput && Input.GetButton("Fire2");f.SprintHeld=f.HasInput && Input.GetButton("Sprint");
            if(f.HasInput && !f.Dead){
                if(Input.GetButtonDown("Weapon1"))f.Commands|=SessionCommand.Slot1;if(Input.GetButtonDown("Weapon2"))f.Commands|=SessionCommand.Slot2;if(Input.GetButtonDown("Weapon3"))f.Commands|=SessionCommand.Unarmed;
                if(Input.GetButtonDown("Flashlight"))f.Commands|=SessionCommand.Flashlight;
                if(f.Scroll!=0){f.Commands|=SessionCommand.CycleSlot;f.SlotDirection=f.Scroll<0?1:-1;}
                if(Input.GetButtonDown("Holster")){f.Commands|=SessionCommand.Holster;f.Equipped=!f.Equipped;}
            }
            if(weaponInput){f.Commands|=SessionCommand.WeaponInput;if(Input.GetButtonDown("FireMode"))f.Commands|=SessionCommand.FireMode;f.FirePressed=Input.GetButtonDown("Fire1");f.FireHeld=Input.GetButton("Fire1");f.ReloadHeld=Input.GetButton("Reload");if(Input.GetButtonDown("Inspect"))f.Commands|=SessionCommand.Inspect;if(Input.GetButtonDown("Melee"))f.Commands|=SessionCommand.Melee;}
            else f.Commands|=SessionCommand.ResetReload;break;
        case 1:
            if(f.Invisible)f.PlayerValid=false;if(f.InfiniteAmmo && f.Armed){f.Commands|=SessionCommand.RefillAmmo;f.Ammo=f.Magazine;}break;
        case 2:
            if(f.JustDied)f.Commands|=SessionCommand.Holster|SessionCommand.Died;
            if(f.WantsRespawn){f.Commands|=SessionCommand.Respawn;f.Position=f.RespawnFeet;f.Velocity=Vector3.Zero;f.Camera=f.RespawnFeet+new Vector3(0,f.EyeHeight,0);}
            if(f.Dead && !f.WantsRespawn)f.Commands|=SessionCommand.DeadCamera;break;
        case 4:f.Equipped=!f.Equipped;break;
        case 3:
            if(f.NpcHit){if(f.AliveHit){f.Commands|=SessionCommand.Hitmarker;if(f.Killed)f.Commands|=SessionCommand.KillFeed;}}else f.Commands|=SessionCommand.WorldImpact;break;
        }
    }
}
