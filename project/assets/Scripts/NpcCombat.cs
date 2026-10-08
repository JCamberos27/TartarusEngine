using System.Numerics;
namespace Tartarus.Gameplay;
internal static unsafe class NpcCombat
{
    static float Wrap(float a){while(a>180)a-=360;while(a< -180)a+=360;return a;}
    static void YawPitch(Vector3 d,ref float yaw,ref float pitch){float len=d.Length();if(len<1e-5f)return;d/=len;yaw=MathF.Atan2(d.Z,d.X)*180/MathF.PI;pitch=MathF.Asin(Math.Clamp(d.Y,-1,1))*180/MathF.PI;}
    static float Random(ref NpcCombatFrame f,float low,float high,bool integer=false) {
        NpcServiceFrame r=new() {Operation=integer?1:0,A=new(low,high,0)};((delegate* unmanaged[Cdecl]<nint,NpcServiceFrame*,int>)f.Services)(f.Context,&r);return r.Value;
    }
    static bool FriendInLine(ref NpcCombatFrame f,Vector3 target) {
        NpcServiceFrame r=new() {Operation=2,A=f.Eye,B=target};return ((delegate* unmanaged[Cdecl]<nint,NpcServiceFrame*,int>)f.Services)(f.Context,&r)!=0;
    }
    internal static void Update(ref NpcCombatFrame f) {
        if(f.Operation==2){
            f.Reloading=f.Pumping=false;
            if(f.WeaponActive){var state=new ReadOnlySpan<byte>((void*)f.WeaponState,f.WeaponStateLength);f.Reloading=state.IndexOf("Reload"u8)>=0;f.Pumping=state.SequenceEqual("Pump"u8);}
            Melee(ref f);return;
        }
        if(f.Operation==0){Aim(ref f);return;}
        Trigger(ref f);
    }
    static void Aim(ref NpcCombatFrame f) {
        f.AimGun=f.Intent.Aim && !f.Reloading && !f.Intent.BlindFire && f.Now-f.MeleeAt>=f.MeleeTime;
        Vector3 target=f.Intent.Aim?f.Intent.AimPoint:f.Intent.LookPoint;
        float ly=f.LookYaw,lp=f.LookPitch;YawPitch(target-f.Eye,ref ly,ref lp);float k=1-MathF.Exp(-Math.Max(f.Dt,0)/.12f);
        f.LookYaw=Wrap(f.LookYaw+Wrap(ly-f.LookYaw)*k);f.LookPitch+=(Math.Clamp(lp,-60,60)-f.LookPitch)*k;
        float wantYaw=f.AimYaw,wantPitch=f.AimPitch;YawPitch(target-f.Eye,ref wantYaw,ref wantPitch);
        if(f.Reloading)wantPitch=Math.Clamp(wantPitch,-15,15);
        else if(!f.Intent.Aim){float pitch=Math.Clamp(wantPitch,-15,10);YawPitch(new(MathF.Sin(f.BodyYaw),0,MathF.Cos(f.BodyYaw)),ref wantYaw,ref wantPitch);wantPitch=pitch;}
        float omega=9+7*f.Skill,dy=Wrap(wantYaw-f.AimYaw),dp=wantPitch-f.AimPitch;
        f.AimYawRate+=(omega*omega*dy-2*omega*f.AimYawRate)*f.Dt;f.AimPitchRate+=(omega*omega*dp-2*omega*f.AimPitchRate)*f.Dt;
        f.AimYaw=Wrap(f.AimYaw+f.AimYawRate*f.Dt);f.AimPitch=Math.Clamp(f.AimPitch+f.AimPitchRate*f.Dt,-80,80);
        var aimDir=NpcPerception.Front(f.AimYaw,f.AimPitch);var wantDir=Vector3.Normalize(target-f.Eye+new Vector3(1e-5f));
        f.Error=MathF.Acos(Math.Clamp(Vector3.Dot(aimDir,wantDir),-1,1))*180/MathF.PI;
        if(f.Visible && f.AimGun && f.Error<6)f.TimeOnTarget+=f.Dt;else f.TimeOnTarget=Math.Max(0,f.TimeOnTarget-f.Dt*2);
    }
    static void Trigger(ref NpcCombatFrame f) {
        f.ToggleAuto=!f.FullAutoSet && f.AllowFullAuto && !f.IsFullAuto;f.FullAutoSet=true;
        if(f.AmmoSeen>=0 && f.Ammo<f.AmmoSeen) {
            int spent=f.AmmoSeen-f.Ammo;f.ShotsFired+=spent;f.LastShot=f.Now;f.FirstShot=false;if(f.BurstLeft>0)f.BurstLeft=Math.Max(0,f.BurstLeft-spent);f.GunNoise=true;
        }
        f.AmmoSeen=f.Ammo;f.BurstPause=Math.Max(0,f.BurstPause-f.Dt);
        f.Sprinting=f.Intent.Pace==3 && f.Intent.Move && f.Velocity.Length()>3.5f;
        Vector3 target=f.Intent.Aim?f.Intent.AimPoint:f.Intent.LookPoint;
        float tolerance=f.Intent.Suppress?9:4.5f+3*Math.Clamp(8/Math.Max(Vector3.Distance(target,f.Eye),1),0,1);
        bool canFire=f.Intent.Fire && !f.HoldFire && !f.Dummy && f.HasAttackToken && f.ReactionLeft<=0 && f.Error<tolerance && !f.Reloading && !f.Signalling && f.Now-f.MeleeAt>=f.MeleeTime && !f.Sprinting && f.Equipped && f.Player.Valid && !f.Player.Dead && (f.Visible || f.Intent.Suppress) && f.Ammo>0;
        if(canFire && FriendInLine(ref f,target)){canFire=false;f.BlockedTime+=.3f;}
        bool pressed=false,held=false;
        if(!f.Shotgun) {
            if(f.BurstLeft==0 && f.BurstPause<=0 && canFire){f.BurstLeft=(int)Random(ref f,f.Intent.Suppress?5:3,f.Intent.Suppress?9:6,true);pressed=true;}
            if(f.BurstLeft>0 && canFire)held=true;else if(f.BurstLeft>0)f.BurstLeft=0;
            if(f.TriggerHeld && !held)f.BurstPause=Random(ref f,f.Intent.Suppress?.25f:.35f,f.Intent.Suppress?.5f:.85f);
        } else if(canFire && f.BurstPause<=0 && f.Chambered){pressed=held=true;f.BurstPause=Random(ref f,.5f,.95f);}
        f.TriggerHeld=held;
        if(f.Reloading && f.Shotgun && f.Ammo>0 && f.Known && f.Player.Valid && !f.Player.Dead && new Vector2(f.Player.Feet.X-f.Feet.X,f.Player.Feet.Z-f.Feet.Z).Length()<2.5f)pressed=true;
        f.Pressed=pressed;f.Held=held;f.Reload=(f.Intent.Reload || f.Ammo==0) && !f.Reloading && !held;
        f.GunVelocity=new Vector2(f.Velocity.X,f.Velocity.Z).Length()>.25f?f.Velocity:Vector3.Zero;
    }
    static void Melee(ref NpcCombatFrame f) {
        var to=new Vector3(f.Player.Feet.X-f.Feet.X,0,f.Player.Feet.Z-f.Feet.Z);float distance=to.Length();var forward=new Vector3(MathF.Sin(f.BodyYaw),0,MathF.Cos(f.BodyYaw));
        float facing=distance>1e-3f?MathF.Acos(Math.Clamp(Vector3.Dot(forward,to/distance),-1,1))*180/MathF.PI:0;
        bool level=Math.Abs(f.Player.Feet.Y-f.Feet.Y)<1;
        if(!f.MeleeLanded && f.Now-f.MeleeAt>=f.MeleeHitTime) {
            f.MeleeLanded=true;
            if(f.Player.Valid && !f.Player.Dead && level && distance<2.3f && facing<70){f.Damage=f.MeleeDamage*Math.Clamp(f.Difficulty,.5f,1.5f);f.DamagePoint=f.Player.Feet+new Vector3(0,f.Player.Height*.75f,0);f.DamageDirection=distance>1e-3f?to/distance:forward;f.MeleeHits++;}
        }
        if(f.HoldFire || f.Dummy || f.Wounded || f.Reloading || !f.Known || !f.Player.Valid || f.Player.Dead || !level || f.Now-f.MeleeAt<f.MeleeTime)return;
        var rule=new AiRuleFrame {Operation=8,Distance=distance,Facing=facing,SinceStrike=f.Now-f.MeleeAt,Reach=1.9f,Cooldown=1.5f};AiRules.Update(ref rule);if(rule.Result==0)return;
        f.MeleeAt=f.Now;f.MeleeLanded=false;f.BurstLeft=0;f.Melees++;f.CallMelee=true;
    }
}
