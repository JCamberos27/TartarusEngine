using System.Numerics;
namespace Tartarus.Gameplay;
internal static class NpcShotEffects
{
    internal static void Update(ref NpcShotEffectFrame f){
        f.Commands=0;
        switch(f.Operation){
        case 0:
            if(f.FirstPellet){f.Commands|=1;if(!f.Shotgun && f.Tracer++%3==0)f.Commands|=2;}
            if(!f.Player.Valid || f.Player.Dead || f.HitPlayer)return;Vector3 segment=f.End-f.Origin;float length=segment.LengthSquared();if(length<1e-4f)return;
            float u=Math.Clamp(Vector3.Dot(f.Player.Eye-f.Origin,segment)/length,0,1);f.Closest=f.Origin+segment*u;f.Miss=Vector3.Distance(f.Closest,f.Player.Eye);
            if(f.Miss<f.Reach && u*MathF.Sqrt(length)>3 && u<.999f)f.Commands|=4;break;
        case 1:
            if(f.Reloading && !f.WasReloading)f.Commands|=8;if(f.Pumping && !f.WasPumping)f.Commands|=16;
            f.ReloadPosition=f.Eye-new Vector3(0,.4f,0);f.PumpPosition=f.Eye-new Vector3(0,.3f,0);f.WasReloading=f.Reloading;f.WasPumping=f.Pumping;break;
        case 2:
            if(f.Friendly){f.Suppression=Math.Min(1,f.Suppression+.3f);f.Damage*=.5f;f.Commands|=32|64;}
            else if(f.Player.Valid && !f.Player.Dead){f.Damage*=f.DamageScale;f.Commands|=64|128|256;}break;
        }
    }
}
