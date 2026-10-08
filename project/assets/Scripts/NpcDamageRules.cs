using System.Numerics;

namespace Tartarus.Gameplay;

internal static class NpcDamageRules
{
    internal static void Update(ref NpcDamageFrame f) {
        HealthRules.State? health=f.UseHealth!=0?HealthRules.Find((uint)f.Entity):null;
        if(f.UseHealth!=0) {
            if(health==null || !health.Enabled){f.Ignore=1;return;}
            f.Health=health.Current;f.MaxHealth=health.Max;
            if(f.Operation==2){f.Kill=f.Health<=0?1:0;return;}
            if(health.Invulnerable && f.Operation==0){f.Ignore=1;return;}
        }
        if(f.Operation==1) {f.BecomeWounded=f.Random<f.WoundChance?1:0;f.CallWounded=f.Health<.35f*f.MaxHealth && f.Wounded==0 && f.BecomeWounded==0?1:0;return;}
        f.Ignore=f.Dead!=0 || f.Amount<=0?1:0;if(f.Ignore!=0)return;
        int region=f.Part>=0?(f.Part==2?0:f.Part is >=3 and <=6?2:f.Part is >=7 and <=10?3:1):(f.Zone==0?0:f.Zone==2?3:1);
        f.Health-=f.Amount;if(health!=null)health.Current=Math.Max(0,f.Health);f.LastHurt=f.Now;f.LastHurtFrom=-f.Direction;f.Hits++;
        f.Suppression=Math.Min(1,f.Suppression+.5f);f.TimeOnTarget*=.3f;
        if(f.Attacker<0) {
            bool known=f.Known!=0;f.Known=1;f.Awareness=1;
            if(f.Visible==0) {f.LastKnown=f.PlayerFeet;f.LastSeen=f.Now;f.Uncertainty=1.5f;}
            if(!known)f.ReactionLeft=Math.Max(f.ReactionLeft,.35f);
        }
        f.Kill=f.Health<=0 || (f.Zone==0 && f.Attacker<0 && f.Amount>=60) || (f.Wounded!=0 && f.Attacker<0)?1:0;
        f.Shove=30+.4f*f.Amount;if(f.Kill!=0)return;
        f.Flinch=1;f.NextThink=Math.Min(f.NextThink,f.Now);
        if(region==3)f.LimpUntil=f.Now+f.LimpTime;
        if(f.Attacker<0 && f.Amount>=f.HeavyHitDamage) {
            f.StaggerUntil=f.Now+f.StaggerTime;f.ReactionLeft=Math.Max(f.ReactionLeft,f.StaggerTime);
            Vector3 flat=new(f.Direction.X,0,f.Direction.Z);if(flat.LengthSquared()>1e-6f)f.PushVelocity+=Vector3.Normalize(flat)*1.8f;
            if(f.PushVelocity.Length()>2.5f)f.PushVelocity=Vector3.Normalize(f.PushVelocity)*2.5f;
        }
        f.NeedRandom=f.Attacker<0 && f.Dummy==0 && f.WoundRolled==0 && f.Health<.2f*f.MaxHealth && region is 1 or 3?1:0;
        if(f.NeedRandom!=0)f.WoundRolled=1;
        f.CallWounded=f.Health<.35f*f.MaxHealth && f.Wounded==0?1:0;
    }
}
