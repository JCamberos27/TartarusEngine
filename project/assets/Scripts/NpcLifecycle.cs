using System.Numerics;
using System.Runtime.InteropServices;
using System.Text.Json;
namespace Tartarus.Gameplay;
internal static unsafe class NpcLifecycle
{
    static float Random(nint services,nint context){NpcServiceFrame r=default;r.Operation=0;r.A=new(0,1,0);((delegate* unmanaged[Cdecl]<nint,NpcServiceFrame*,int>)services)(context,&r);return r.Value;}
    static Vector3 Front(float yaw,float pitch=0){float y=yaw*MathF.PI/180,p=pitch*MathF.PI/180;return new(MathF.Cos(y)*MathF.Cos(p),MathF.Sin(p),MathF.Sin(y)*MathF.Cos(p));}
    internal static string Request(string op,string json){
        if(op=="npc.name")return JsonSerializer.Serialize("Soldier "+JsonSerializer.Deserialize<int>(json));
        using var doc=JsonDocument.Parse(json);var x=doc.RootElement;
        if(op=="npc.spawn-plan")return JsonSerializer.Serialize(new {count=Math.Min(x.GetProperty("size").GetInt32(),x.GetProperty("spawns").GetInt32()*2),spares=x.GetProperty("respawn").GetBoolean() && x.GetProperty("body").GetBoolean()?2:0});
        throw new ArgumentException(op);
    }
    internal static void Spawn(ref NpcSpawnFrame f){
        var members=(NpcMateFrame*)f.Members;f.Feet=f.Position;
        for(int tries=0;tries<8;tries++){bool crowded=false;for(int i=0;i<f.Count;i++)if(!members[i].Dead && Vector3.Distance(members[i].Feet,f.Feet)<1){crowded=true;break;}if(!crowded)break;float a=tries*2.4f;f.Feet=f.Position+new Vector3(MathF.Cos(a),0,MathF.Sin(a))*(1.2f+.3f*tries);}
        if(f.Weapon==2)f.Weapon=(int)((uint)f.Random%2);f.Radius=.3f;f.StandCylinder=.6f;f.AgentRadius=.34f;f.AgentHeight=1.8f;f.JogSpeed=3.2f;f.RunSpeed=4.7f;
        f.AimYaw=MathF.Atan2(MathF.Cos(f.Yaw),MathF.Sin(f.Yaw))*180/MathF.PI;f.NextLook=f.Now+.05f*f.Index;f.NextThink=f.Now+.1f;f.Eye=f.Feet+new Vector3(0,1.62f,0);f.LookPoint=f.Eye+Front(f.AimYaw)*10;
    }
    internal static void Life(ref NpcLifeFrame f){
        f.Kill=f.Despawn=f.CallManDown=false;f.StandCylinder=.6f;f.CrouchCylinder=.33f;
        switch(f.Operation){
        case 0:
            f.Kill=f.Wounded && f.Now-f.WoundedAt>f.BleedOutTime;f.Despawn=f.Dead && f.Now-f.DiedAt>f.CorpseTime;
            f.Crouch=f.Intent.Crouch && f.Intent.Pace!=3;
            f.Speed=f.Crouch?(f.Intent.Pace==0?0:1.9f):f.Intent.Pace switch {0=>0,1=>1.5f,2=>3.2f,3=>4.7f,_=>3.2f};
            if(f.Wounded)f.Speed=f.CrawlSpeed;else if(f.Now<f.LimpUntil)f.Speed*=f.LimpScale;if(f.Now<f.StaggerUntil)f.Speed*=.2f;
            break;
        case 1:
            f.Want.Y=0;f.FallSpeed-=f.Gravity*f.Dt;f.Displacement=(f.Want+f.Push)*f.Dt+new Vector3(0,f.FallSpeed*f.Dt,0);f.Push*=MathF.Exp(-7*f.Dt);break;
        case 2:
            if(f.Grounded)f.FallSpeed=-1;
            f.BlockedTime=f.Intent.Move && f.Want.Length()>.5f && f.Velocity.Length()<.25f?f.BlockedTime+f.Dt:Math.Max(0,f.BlockedTime-f.Dt*2);
            f.Crouch=f.Intent.Crouch && f.Intent.Pace!=3;f.Aim=f.Intent.Aim && !f.Intent.BlindFire && f.Now-f.MeleeAt>=f.MeleeTime;f.Sprint=f.Intent.Pace==3 && f.Intent.Move;
            f.FootIK=f.FootIKEverywhere || f.PlayerDistance<f.FootIKRange && f.OnScreen;break;
        case 3:
            Vector3 segment=f.End-f.Origin;float length=segment.LengthSquared();if(length<1e-4f)break;
            float t=Math.Clamp(Vector3.Dot(f.Eye-f.Origin,segment)/length,0,1),distance=Vector3.Distance(f.Origin+segment*t,f.Eye);
            if(distance>=1.6f || t<=.02f)break;f.Suppression=Math.Min(1,f.Suppression+.22f*(1.6f-distance));
            if(distance<1 && f.Now>f.CowerUntil+.8f && Random(f.Services,f.Context)<.65f-.35f*f.Skill)f.CowerUntil=f.Now+.35f+.3f*Random(f.Services,f.Context);
            if(!f.Memory.Known){f.Memory.Known=true;f.Memory.Awareness=1;f.Memory.LastKnown=f.Origin;f.Memory.LastSeen=f.Now;f.Memory.Uncertainty=3;}break;
        case 5:f.Memory=default;f.Memory.Awareness=.3f;break;
        case 4:f.Morale=Math.Max(0,f.Morale-.25f);f.CallManDown=Vector3.Distance(f.Feet,f.Origin)<25;break;
        }
    }
    internal static void Noise(ref NpcPlayerNoiseFrame f){
        f.Count=0;if(!f.Player.Valid || f.Player.Dead)return;
        if(f.Player.Fired)f.Gun=new(){Position=f.Player.Eye,Radius=85,Loudness=1,Time=f.Now,Source=-1};
        f.FootstepTimer-=f.Dt;float speed=new Vector2(f.Player.Velocity.X,f.Player.Velocity.Z).Length();
        if(speed>1 && !f.Player.Crouched && f.FootstepTimer<=0){bool sprint=speed>4.5f;f.Step=new(){Position=f.Player.Feet,Radius=sprint?16:7,Loudness=sprint?.35f:.18f,Time=f.Now,Source=-1};f.FootstepTimer=sprint?.32f:.45f;}
        if(f.Player.Reloading)f.Reload=new(){Position=f.Player.Eye,Radius=9,Loudness=.15f,Time=f.Now,Source=-2};
        if(Vector3.Distance(f.Player.Feet,f.Post)>2.5f){f.Post=f.Player.Feet;f.Still=0;}else f.Still+=f.Dt;
    }
    internal static void Respawn(ref NpcRespawnFrame f){
        f.Discard=f.Alive>=f.Wanted;f.Ready=f.Now>=f.Timer;f.Best=-1;f.Timer=f.Now+2;
        if(!f.Ready || f.Discard)return;var points=(NpcRespawnPointFrame*)f.Points;float best=-1e9f;
        for(int i=0;i<f.Count;i++){float distance=f.Player.Valid?Vector3.Distance(points[i].Position+new Vector3(0,1.6f,0),f.Player.Eye):50;
            float score=distance+(points[i].Seen?-40:0)+(distance<20?-100:0);if(score>best){best=score;f.Best=i;}}
    }
    static float Smooth(float x){x=Math.Clamp(x,0,1);return x*x*(3-2*x);}
    internal static void Pose(ref NpcShotPoseFrame f){
        Vector3 front=Front(f.AimYaw);if(f.Intent.BlindFire && f.Alive){f.BlindOffset=new Vector3(0,.34f,0)+front*.12f;var across=f.Peek-f.CoverPosition;across.Y=0;if(f.HighCover && across.LengthSquared()>1e-4f)f.BlindOffset=Vector3.Normalize(across)*.45f+front*.1f+new Vector3(0,.12f,0);}
        f.BlindLift+=((f.Intent.BlindFire && f.Alive?1:0)-f.BlindLift)*(1-MathF.Exp(-f.Dt/.09f));f.CameraOffset=f.BlindLift>1e-3f?f.BlindOffset*f.BlindLift:Vector3.Zero;f.Roll=0;
        float mt=(f.Now-f.MeleeAt)/f.MeleeTime;if(f.Alive && mt>=0 && mt<1){float reach=mt<.3f?-.12f*Smooth(mt/.3f):mt<.42f?-.12f+.52f*Smooth((mt-.3f)/.12f):.4f*(1-Smooth((mt-.42f)/.58f));f.CameraOffset+=Front(f.AimYaw,f.AimPitch)*reach;f.Roll=28*MathF.Sin(MathF.PI*mt);}
        f.HaveShot=f.Alive && f.TriggerHeld;if(!f.HaveShot)return;
        nint services=f.Services,context=f.Context;float Rand()=>Random(services,context);
        if(f.Intent.Suppress && !f.Memory.Visible || f.Intent.BlindFire){var memory=new AiRuleFrame{Operation=1,Position=f.Memory.LastKnown,Velocity=f.Memory.LastVelocity,Now=f.Now,LastSeen=f.Memory.LastSeen};AiRules.Update(ref memory);f.Shot=memory.Position+new Vector3(0,1.1f,0)+new Vector3(Rand()-.5f,Rand()*.8f,Rand()-.5f)*(.8f+f.Memory.Uncertainty*.3f)*(f.Intent.BlindFire?2.4f:1);return;}
        var accuracy=new AiRuleFrame{Operation=4,Distance=Vector3.Distance(f.Player.Eye,f.Eye),Speed=new Vector2(f.Player.Velocity.X,f.Player.Velocity.Z).Length(),TimeOnTarget=f.TimeOnTarget,SelfSpeed=f.Velocity.Length(),Suppression=f.Suppression,Skill=f.Skill,Difficulty=f.Difficulty,VisibleFraction=f.VisiblePoints/5f,Crouched=f.Player.Crouched?1:0,OutsideView=Vector3.Dot(Vector3.Normalize(f.Eye-f.Player.Eye+new Vector3(1e-5f)),f.Player.Forward)<MathF.Cos(55*MathF.PI/180)?1:0,Flinching=f.Now-f.LastHurt<.35f?1:0,Weapon=f.Weapon};AiRules.Update(ref accuracy);
        bool hit=!f.FirstShot && Rand()<accuracy.Result*(f.Wounded?.5f:1);Vector3 seen=f.VisiblePoints>0?f.SeenPoint:f.Player.Feet+new Vector3(0,f.Player.Height*.7f,0);
        if(hit)f.Shot=seen+new Vector3(Rand()-.5f,Rand()-.5f,Rand()-.5f)*.18f;
        else {Vector3 across=Vector3.Normalize(Vector3.Cross(f.Player.Eye-f.Eye,Vector3.UnitY)+new Vector3(1e-5f));float side=(Rand()<.5f?-1:1)*(.45f+.7f*Rand());f.Shot=seen+across*side+new Vector3(0,-.2f+.7f*Rand(),0);}
    }
}
