using System.Numerics;

namespace Tartarus.Gameplay;

internal static unsafe class NpcPerception
{
    internal static Vector3 Front(float yaw,float pitch) {
        yaw*=MathF.PI/180;pitch*=MathF.PI/180;return new(MathF.Cos(yaw)*MathF.Cos(pitch),MathF.Sin(pitch),MathF.Sin(yaw)*MathF.Cos(pitch));
    }
    internal static AiRuleFrame Memory(NpcMemoryFrame m)=>new() {Known=m.Known?1:0,Visible=m.Visible?1:0,Awareness=m.Awareness,LastSeen=m.LastSeen,LastHeard=m.LastHeard,Uncertainty=m.Uncertainty,Position=m.LastKnown,Velocity=m.LastVelocity};
    internal static void Memory(ref NpcMemoryFrame m,AiRuleFrame a) {
        m.Known=a.Known!=0;m.Visible=a.Visible!=0;m.Awareness=a.Awareness;m.LastSeen=a.LastSeen;m.LastHeard=a.LastHeard;m.Uncertainty=a.Uncertainty;m.LastKnown=a.Position;m.LastVelocity=a.Velocity;
    }
    static float Reaction(float skill,float difficulty,bool peripheral,float random) {
        var a=new AiRuleFrame {Operation=5,Skill=skill,Difficulty=difficulty,OutsideView=peripheral?1:0,Random=random};AiRules.Update(ref a);return a.Result;
    }
    static float Random(ref NpcPerceptionFrame f) {
        NpcServiceFrame r=new() {Operation=0};((delegate* unmanaged[Cdecl]<nint,NpcServiceFrame*,int>)f.Services)(f.Context,&r);return r.Value;
    }
    static bool Visible(ref NpcPerceptionFrame f,Vector3 target) {
        NpcServiceFrame r=new() {Operation=1,A=f.SightEye,B=target};return ((delegate* unmanaged[Cdecl]<nint,NpcServiceFrame*,int>)f.Services)(f.Context,&r)!=0;
    }
    internal static void Update(ref NpcPerceptionFrame f) {
        f.Suppression=Math.Max(0,f.Suppression-f.Dt*.25f);f.ReactionLeft=Math.Max(0,f.ReactionLeft-f.Dt);
        if(f.Suppression>.5f) {if(f.PinnedSince<0)f.PinnedSince=f.Now;}else if(f.Suppression<.3f)f.PinnedSince=-1;
        var look=f.Aim?Front(f.AimYaw,f.AimPitch):Front(f.LookYaw,f.LookPitch);
        f.SightEye=f.Eye+new Vector3(0,.08f,0)+Vector3.Normalize(new Vector3(look.X,0,look.Z)+new Vector3(1e-5f))*.08f;
        var noises=(NpcNoiseFrame*)f.Noises;var mates=(NpcPerceptionMateFrame*)f.Mates;
        for(int i=0;i<f.NoiseCount;i++) {
            ref var z=ref noises[i];if(z.Source==f.Index || f.Now-z.Time>f.Dt+1e-4f)continue;
            if(z.Source>=0) {
                if(z.Source<f.MateCount) {
                    ref var o=ref mates[z.Source];
                    if(o.Exists && o.Squad==f.Squad && o.Memory.Known && Vector3.Distance(z.Position,f.SightEye)<z.Radius) {
                        f.Memory.Awareness=1;if(!f.Memory.Known){f.Memory.Known=true;f.Calls|=1;}
                        if(o.Memory.LastSeen>f.Memory.LastSeen) {f.Memory.LastKnown=o.Memory.LastKnown;f.Memory.LastSeen=o.Memory.LastSeen;f.Memory.Uncertainty=Math.Max(f.Memory.Uncertainty,o.Memory.Uncertainty+1);}
                    }
                }
                continue;
            }
            bool wasKnown=f.Memory.Known;var a=Memory(f.Memory);a.Operation=3;a.Listener=f.SightEye;a.Source=z.Position;a.Radius=z.Radius;a.Rate=z.Loudness;a.Now=f.Now;AiRules.Update(ref a);Memory(ref f.Memory,a);
            if(!wasKnown && f.Memory.Known) {f.ReactionLeft=Math.Max(f.ReactionLeft,Reaction(f.Skill,f.Difficulty,true,.5f));f.Calls|=2;}
        }
        if(f.Now<f.NextLook)return;
        float lookDt=Math.Clamp(f.Now-(f.NextLook-.12f),0,.5f);f.NextLook=f.Now+.1f+.04f*Random(ref f);
        int visible=0;Vector3 best=Vector3.Zero;float angle=180,distance=1e9f;
        ref var p=ref f.Player;
        if(p.Valid && !p.Dead) {
            var to=p.Eye-f.SightEye;distance=to.Length();
            var flatLook=Vector3.Normalize(new Vector3(look.X,0,look.Z)+new Vector3(1e-5f,0,0));
            angle=MathF.Acos(Math.Clamp(Vector3.Dot(Vector3.Normalize(to),look),-1,1))*180/MathF.PI;
            float flatAngle=MathF.Acos(Math.Clamp(Vector3.Dot(Vector3.Normalize(new Vector3(to.X,0,to.Z)+new Vector3(1e-5f,0,0)),flatLook),-1,1))*180/MathF.PI;
            angle=Math.Min(angle,flatAngle);
            if(distance<90 && angle<80) {
                var right=Vector3.Normalize(Vector3.Cross(Vector3.Normalize(new Vector3(to.X,0,to.Z)+new Vector3(1e-5f,0,0)),Vector3.UnitY));
                var chest=p.Feet+new Vector3(0,p.Height*.72f,0);
                Span<Vector3> points=stackalloc Vector3[5] {chest,p.Eye-new Vector3(0,.04f,0),p.Feet+new Vector3(0,p.Height*.45f,0),chest+right*.2f,chest-right*.2f};
                foreach(var q in points)if(Vector3.Distance(q,f.SightEye)>=1e-3f && Visible(ref f,q)) {if(visible==0)best=q;visible++;}
            }
        }
        f.VisiblePoints=visible;if(visible>0){f.SeenPoint=best;f.LastOwnSight=f.Now;}
        var detect=new AiRuleFrame {Operation=0,Distance=distance,Angle=angle,VisiblePoints=visible,Speed=new Vector2(p.Velocity.X,p.Velocity.Z).Length(),Crouched=p.Crouched?1:0,Firing=p.Fired?1:0,Suppression=f.Suppression,Alertness=f.Memory.Known?1:f.Doing==1?.6f:0,Focal=30,Peripheral=80,Range=90,BaseRate=2.4f};AiRules.Update(ref detect);
        bool wasVisible=f.Memory.Visible;float lastSeen=f.Memory.LastSeen;var memory=Memory(f.Memory);memory.Operation=2;memory.Visible=visible>0?1:0;memory.Source=p.Feet;memory.Listener=p.Velocity;memory.Rate=detect.Result;memory.Now=f.Now;memory.Dt=lookDt;AiRules.Update(ref memory);Memory(ref f.Memory,memory);
        if(memory.BecameKnown!=0) {
            f.ReactionLeft=Reaction(f.Skill,f.Difficulty,angle>30,Random(ref f));f.FirstShot=true;
            if((distance<12 || angle>35) && Random(ref f)<.75f-.45f*f.Skill) {f.CowerUntil=f.Now+.3f;f.ReactionLeft=Math.Max(f.ReactionLeft,.35f);f.Startles++;}
            f.Calls|=4;
        } else if(f.Memory.Visible && !wasVisible && f.Memory.Known) {
            if(f.Now-lastSeen>1.5f) {f.ReactionLeft=Math.Max(f.ReactionLeft,.6f*Reaction(f.Skill,f.Difficulty,angle>30,.5f));f.FirstShot=f.Now-lastSeen>4;}
            f.TimeOnTarget=0;
        }
    }
}
