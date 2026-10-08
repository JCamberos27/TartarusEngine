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
                if(Input.GetButtonDown("Weapon1"))f.Commands|=1;if(Input.GetButtonDown("Weapon2"))f.Commands|=2;if(Input.GetButtonDown("Weapon3"))f.Commands|=4;
                if(Input.GetButtonDown("Flashlight"))f.Commands|=131072;
                if(f.Scroll!=0){f.Commands|=8;f.SlotDirection=f.Scroll<0?1:-1;}
                if(Input.GetButtonDown("Holster")){f.Commands|=16;f.Equipped=!f.Equipped;}
            }
            if(weaponInput){f.Commands|=32;if(Input.GetButtonDown("FireMode"))f.Commands|=64;f.FirePressed=Input.GetButtonDown("Fire1");f.FireHeld=Input.GetButton("Fire1");f.ReloadHeld=Input.GetButton("Reload");if(Input.GetButtonDown("Inspect"))f.Commands|=128;if(Input.GetButtonDown("Melee"))f.Commands|=256;}
            else f.Commands|=512;break;
        case 1:
            if(f.Invisible)f.PlayerValid=false;if(f.InfiniteAmmo && f.Armed){f.Commands|=1024;f.Ammo=f.Magazine;}break;
        case 2:
            if(f.JustDied)f.Commands|=16|2048;
            if(f.WantsRespawn){f.Commands|=4096;f.Position=f.RespawnFeet;f.Velocity=Vector3.Zero;f.Camera=f.RespawnFeet+new Vector3(0,f.EyeHeight,0);}
            if(f.Dead && !f.WantsRespawn)f.Commands|=8192;break;
        case 4:f.Equipped=!f.Equipped;break;
        case 3:
            if(f.NpcHit){if(f.AliveHit){f.Commands|=16384;if(f.Killed)f.Commands|=32768;}}else f.Commands|=65536;break;
        }
    }
}
