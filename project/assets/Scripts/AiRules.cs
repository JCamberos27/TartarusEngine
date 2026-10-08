using System.Numerics;

namespace Tartarus.Gameplay;

internal static unsafe class AiRules
{
    static float Clamp(float x,float low=0,float high=1)=>Math.Clamp(x,low,high);
    static float Smooth(float x) {x=Clamp(x);return x*x*(3-2*x);}
    internal static void Update(ref AiRuleFrame f) {
        switch(f.Operation) {
        case 0:
            if(f.VisiblePoints<=0 || f.Distance>f.Range || f.Angle>f.Peripheral) {f.Result=0;break;}
            float distance=1-.94f*MathF.Sqrt(Clamp((f.Distance-8)/Math.Max(f.Range-8,1)));
            float angle=f.Angle>f.Focal?.45f-.30f*Clamp((f.Angle-f.Focal)/Math.Max(f.Peripheral-f.Focal,1)):1;
            float visible=.3f+.7f*Clamp(f.VisiblePoints/5f);
            float stance=f.Crouched!=0 && f.Firing==0?.6f:1;
            f.Result=f.BaseRate*distance*angle*visible*stance*(.8f+.25f*Clamp(f.Speed,0,5))*(f.Firing!=0?4:1)*(1-.5f*Clamp(f.Suppression))*(1+1.5f*Clamp(f.Alertness));break;
        case 1:f.Position+=new Vector3(f.Velocity.X,0,f.Velocity.Z)*Clamp(f.Now-f.LastSeen,0,1.5f);break;
        case 2:
            f.Visible=f.Visible!=0 && f.Rate>0?1:0;f.BecameKnown=0;
            if(f.Visible!=0) {
                f.Awareness=Math.Min(1,f.Awareness+f.Rate*f.Dt);
                if(f.Awareness>=1 && f.Known==0) {f.Known=f.BecameKnown=1;}
                if(f.Known!=0 || f.Awareness>.35f) {f.Position=f.Source;f.Velocity=f.Listener;f.LastSeen=f.Now;f.Uncertainty=f.Known!=0?0:2*(1-f.Awareness);}
            } else if(f.Known==0)f.Awareness=Math.Max(0,f.Awareness-.12f*f.Dt);
            else if(f.LastSeen>-1e8f)f.Uncertainty=Math.Min(14,f.Uncertainty+1.3f*f.Dt);
            break;
        case 3:
            float d=Vector3.Distance(f.Source,f.Listener);if(f.Radius<=0 || d>f.Radius) {f.Result=0;break;}
            float before=f.Awareness;f.Awareness=Math.Min(1,f.Awareness+f.Rate*(.35f+.65f*(1-d/f.Radius)));
            if(f.Awareness>=1)f.Known=1;f.LastHeard=f.Now;
            float vague=.5f+d*.12f;
            if(f.Now-f.LastSeen>1 || f.Uncertainty>vague) {f.Position=f.Source;f.Velocity=Vector3.Zero;f.Uncertainty=vague;if(f.Known==0)f.LastSeen=Math.Max(f.LastSeen,f.Now-.01f);}
            f.Result=f.Awareness-before;break;
        case 4:
            float skill=Clamp(f.Skill),p=.42f+.38f*skill;
            p*=MathF.Exp(-Math.Max(f.Distance-6,0)/(f.Weapon==1?16:48));
            p*=.3f+.7f*Smooth(f.TimeOnTarget/(1.4f-.5f*skill));
            p/=1+.16f*Math.Max(f.Speed,0);p/=1+.35f*Math.Max(f.SelfSpeed,0);
            p*=1-.65f*Clamp(f.Suppression);p*=.35f+.65f*Clamp(f.VisibleFraction);
            if(f.Crouched!=0)p*=.85f;if(f.OutsideView!=0)p*=.5f;if(f.Flinching!=0)p*=.35f;
            f.Result=Clamp(p*Clamp(f.Difficulty,.25f,2),0,.85f);break;
        case 5:f.Result=(.42f-.2f*Clamp(f.Skill)+.18f*Clamp(f.Random)+(f.OutsideView!=0?.18f:0))/Clamp(f.Difficulty,.25f,2);break;
        case 6:f.Result=f.Exposed==0 || f.CoverFire!=0 || f.Waited>=f.MaxWait?1:0;break;
        case 7:f.Result=f.KnowsThreat!=0 && f.Suppression>.5f && f.PinnedFor>1.5f && f.SinceSeen<8?1:0;break;
        case 8:f.Result=f.Distance<=f.Reach && f.Facing<50 && f.SinceStrike>=f.Cooldown?1:0;break;
        case 9:
            f.Result=1;
            for(int i=0;i<f.Count;i++) {
                float age=f.Now-f.Times[i];if(age<0 || age>f.Memory)continue;
                float flat=Vector2.Distance(new(f.Position.X,f.Position.Z),new(f.Positions[i].X,f.Positions[i].Z));if(flat>=f.Radius)continue;
                f.Result=Math.Min(f.Result,1-.7f*(1-flat/f.Radius)*(1-age/f.Memory));
            }
            break;
        case 10:
            float x=Clamp(f.Distance),y=0;
            if(f.CurveKind==0)y=f.M*x+f.B;
            if(f.CurveKind==1)y=f.M*MathF.Pow(x,f.K)+f.B;
            if(f.CurveKind==2)y=1/(1+MathF.Exp(-f.K*(x-f.C)))*f.M+f.B;
            if(f.CurveKind==3) {float v=(x-f.C)/Math.Max(f.K,.0001f);y=MathF.Exp(-v*v)*f.M+f.B;}
            f.Result=Clamp(y);break;
        case 11:
            if(f.Count<=0) {f.Result=0;break;}
            float product=1,mod=1-1f/f.Count;
            for(int i=0;i<f.Count;i++) {float s=Clamp(f.Values[i]);product*=s+(1-s)*mod*s;}
            f.Result=Clamp(product);break;
        }
    }
}
