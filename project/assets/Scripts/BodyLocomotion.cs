using System.Numerics;
namespace Tartarus.Gameplay;
// The first-person body's locomotion decisions, each frame: what the gait blend is fed, when a start, stop, pivot,
// step, turn or fidget plays, and how far along its travel each distance-matched clip should be. The capsule moves on
// the input (input-led); nothing here holds the player back - the clips are picked and paced to fit its travel.
internal static class BodyLocomotion
{
    const float Rad=MathF.PI/180;
    const float WalkJogSplit=2.2f;     // clip m/s: below, a walk's start / stop / pivot; above, a jog's
    const float StartTurnMin=40;       // degrees the body is off where it should face before a start turns it
    const float StartTurnTravel=50;    // ... while the travel is within this of that facing
    const float PivotMinSpeed=1.0f, PivotMinSpeedCrouched=.6f; // clip m/s the travel needs for a pivot to read
    const float StepMaxTime=.3f;       // a tap shorter than this is a step, not a start and a stop
    const float SprintWarpMax=50;      // degrees the legs turn into a diagonal sprint (the torso keeps the view)
    const float FidgetMin=10, FidgetMax=20;
    static float Wrap(float a){a%=MathF.Tau;if(a>MathF.PI)a-=MathF.Tau;else if(a<=-MathF.PI)a+=MathF.Tau;return a;}
    static float Follow(float dt,float seconds)=>seconds>1e-4f?1-MathF.Exp(-dt/seconds):1;
    static Vector3 Local(Vector3 v,float yaw)=>new(-v.X*MathF.Cos(yaw)+v.Z*MathF.Sin(yaw),v.X*MathF.Sin(yaw)+v.Z*MathF.Cos(yaw),0);
    static float Heading(Vector3 v)=>MathF.Atan2(v.X,v.Z);
    static float ClipSpeed(float speed,ref BodyMotionFrame f) {
        // Preserve the tuned walk/jog/sprint mapping in metres per second.
        speed=Math.Max(speed,0);float run=Math.Max(f.PlayerRunSpeed,.01f);
        if(speed<=run || f.PlayerSprintSpeed<=run+.001f)return speed*f.RunSpeed/run;
        return f.RunSpeed+(speed-run)*(f.ClipSprint-f.RunSpeed)/(f.PlayerSprintSpeed-run);
    }
    static Vector3 AsClip(Vector3 v,ref BodyMotionFrame f){var local=Local(v,f.Yaw);float s=local.Length();return s>1e-4f?local*(ClipSpeed(s,ref f)/s):Vector3.Zero;}
    static float Gait(float clipSpeed,bool sprint)=>sprint?2:clipSpeed>WalkJogSplit?1:0;
    // How far a capsule easing (time constant tau) from speed v toward speed `target` the other way still goes
    // before it turns: integral of v(t) to its zero crossing.
    internal static float DistanceToTurn(float v,float target,float tau) {
        if(v<=0 || tau<=0)return 0;
        if(target<=1e-3f)return v*tau;
        return Math.Max(0,v*tau-target*tau*MathF.Log(1+v/target));
    }
    internal static void Update(ref BodyMotionFrame f) {
        f.Triggers=0;f.SetTurnAngle=false;
        var moving=new Vector3(f.Velocity.X,0,f.Velocity.Z);float responsiveness=Math.Clamp(f.Responsiveness,0,1);
        var wishWorld=new Vector3(f.WishVelocity.X,0,f.WishVelocity.Z);float wishSpeed=wishWorld.Length();
        float movingSpeed=moving.Length();bool sprint=wishSpeed>f.PlayerRunSpeed*1.05f;
        // Sprinting on a diagonal, the legs square up to the travel and the torso twists back to the view.
        float faceYaw=f.ViewYaw;
        if(sprint && f.Grounded && !f.Crouched && wishSpeed>.1f){float off=Wrap(Heading(wishWorld)-f.ViewYaw);if(MathF.Abs(off)<100*Rad)faceYaw=f.ViewYaw+Math.Clamp(off,-SprintWarpMax*Rad,SprintWarpMax*Rad);}
        var target=Vector3.Lerp(AsClip(f.WishVelocity,ref f),AsClip(moving,ref f),responsiveness);
        float clip=ClipSpeed(movingSpeed,ref f);f.PlayRate=clip>.05f?1+(Math.Clamp(movingSpeed/clip,1,Math.Max(f.MaxPlayRate,1))-1)*Math.Min(clip/.5f,1)*(sprint?1:responsiveness):1;
        f.SprintRate=f.SprintClip>.1f?Math.Clamp(movingSpeed/f.SprintClip,.8f,1.5f):1;
        bool hold=f.StartStopClips && target.Length()<.01f && f.IdleTime<f.StopDebounce && f.Move.Length()>(f.Crouched?f.StopMinSpeedCrouched:f.StopMinSpeed);
        if(!hold)f.Move+=(target-f.Move)*Follow(f.Dt,f.ParamSmoothing);
        f.AirTime=f.Grounded?0:f.AirTime+f.Dt;
        bool still=f.Grounded && wishSpeed<.1f && f.Move.Length()<.2f;f.Still=still && f.TurnThreshold>0;
        float offset=Wrap(faceYaw-f.Yaw);
        if(f.TurnThreshold<=0){f.Yaw=faceYaw;f.Turning=false;}
        else if(f.Turning){
            f.TurnTime+=f.Dt;if(f.IsTurn || f.IsCrouchTurn){float step=f.RootYaw*Rad;f.Yaw+=step;f.TurnDone+=step;}
            offset=Wrap(faceYaw-f.Yaw);bool clipDone=f.TurnTime>f.TurnMinTime && (!f.IsTurn || f.StateTime>=.97f);
            if(!still || Math.Abs(offset)<f.TurnEndAngle*Rad || clipDone || f.TurnTime>f.TurnTimeout)f.Turning=false;
        } else if(still){
            // A turn on the spot is never under 45 degrees of clip: each side's smallest, so the sides never mix.
            if(Math.Abs(offset)>f.TurnThreshold*Rad){f.Turning=true;f.TurnTime=f.TurnDone=0;float deg=Math.Clamp(offset/Rad,-180,180);f.TurnAngle=MathF.CopySign(Math.Max(MathF.Abs(deg),45),deg);f.SetTurnAngle=true;}
        } else if(f.IsStartTurn){
            // A start that turns: the body takes the clip's own yaw, up to where it should face.
            float step=f.RootYaw*Rad;if(MathF.Sign(step)==MathF.Sign(offset))f.Yaw+=MathF.CopySign(Math.Min(MathF.Abs(step),MathF.Abs(offset)),step);
        }
        else f.Yaw+=offset*Follow(f.Dt,f.TurnMoveEase);
        if(f.TurnThreshold>0 && !f.IsStartTurn){float lag=Wrap(faceYaw-f.Yaw),max=Math.Max(f.TurnThreshold+f.TurnLagMargin,f.TurnLagFloor)*Rad;if(Math.Abs(lag)>max)f.Yaw=faceYaw-MathF.CopySign(max,lag);}
        f.Yaw=Wrap(f.Yaw);f.Twist=Wrap(f.ViewYaw-f.Yaw);
        var wish=Local(f.WishVelocity,f.Yaw);float wishLen=wish.Length();f.Moving=wishLen>.1f;
        bool idling=f.IsLocomotion || f.IsCrouchLoco || f.IsStep || f.IsFidget;
        bool plain=f.StartStopClips && f.Grounded && idling && !f.Turning;
        bool going=f.StartStopClips && f.Grounded && !f.Turning && (f.IsLocomotion || f.IsCrouchLoco || f.IsStart || f.IsPivot || f.IsSprint);
        if(f.StartStopClips && f.Grounded && !f.Moving && f.Move.Length()<.4f && f.Crouched!=f.WasCrouched){if(f.Crouched && f.IsLocomotion)f.Triggers|=1;if(!f.Crouched && f.IsCrouchLoco)f.Triggers|=2;}
        f.WasCrouched=f.Crouched;
        float moveClip=f.Move.Length();
        if(f.Moving){
            var dir=wish/wishLen;
            if(plain && f.IdleTime>=f.StartIdleTime && moveClip<f.StartMaxMove){
                f.StartDir=dir;f.StartGait=Gait(ClipSpeed(wishSpeed,ref f),sprint);f.StartTurn=0;f.StartTurnAmount=1;
                // Moving off toward where the body should face but isn't yet (the view whipped round, or a new
                // heading): a start that turns it on the way.
                float lag=Wrap(faceYaw-f.Yaw),travel=Wrap(Heading(wishWorld)-faceYaw);
                if(MathF.Abs(lag)>StartTurnMin*Rad && MathF.Abs(travel)<StartTurnTravel*Rad){
                    float amount=Math.Clamp(MathF.Abs(lag)/(45*Rad),1,4);if(f.Crouched)amount=MathF.Round(amount);
                    f.StartTurn=MathF.CopySign(amount,lag);f.StartTurnAmount=amount;
                }
                f.Triggers|=4;f.MoveDistance=0;
            }
            // The travel reversing at speed: a pivot - the feet plant and push off the other way.
            float pivotMin=f.Crouched?PivotMinSpeedCrouched:PivotMinSpeed;
            if(going && !f.IsPivot && moveClip>pivotMin && Vector3.Dot(dir,f.Move/moveClip)<-.5f){
                f.PivotDir=f.Move/moveClip;f.PivotGait=Gait(moveClip,false);f.PivotReversed=false;f.PivotTravel=0;f.Triggers|=64;
            }
            f.LastDir=dir;f.LastSprint=sprint;f.IdleTime=0;f.MoveTime+=f.Dt;f.MoveDistance+=movingSpeed*f.Dt;
        }
        else {
            float before=f.IdleTime;f.IdleTime+=f.Dt;bool enough=f.MoveTime>=(f.Crouched?f.StopMinRunTimeCrouched:f.StopMinRunTime);
            bool released=before<f.StopDebounce && f.IdleTime>=f.StopDebounce;
            // A tap: one small step its way instead of a start cut short.
            if(released && f.IsStart && !f.Crouched && f.MoveTime<StepMaxTime){f.StepDir=f.LastDir;f.Triggers|=128;}
            else if(going && enough && released && moveClip>(f.Crouched?f.StopMinSpeedCrouched:f.StopMinSpeed)){
                f.StopDir=f.LastDir;f.StopGait=Gait(moveClip,false);f.Triggers|=f.LastSprint && f.LastDir.Y>f.StopRunForward?16:8;
            }
            if(released)f.MoveTime=0;
        }
        // Distance matching: how far the capsule has gone since the start, still has to go in the stop, and is from
        // the pivot's turnaround (negative until its travel the old way runs out).
        f.StartDistance=f.IsStart?f.StartDistance+movingSpeed*f.Dt:0;
        f.StopDistance=movingSpeed*f.DecelTime;
        if(f.IsPivot){
            var local=Local(moving,f.Yaw);float along=local.X*f.PivotDir.X+local.Y*f.PivotDir.Y;
            if(!f.PivotReversed && along>.05f)f.PivotDistance=-DistanceToTurn(along,wishSpeed,f.AccelTime);
            else {f.PivotReversed=true;f.PivotTravel+=movingSpeed*f.Dt;f.PivotDistance=f.PivotTravel;}
        } else if((f.Triggers&64)==0){f.PivotDistance=0;f.PivotReversed=false;}
        // Idle variety: after a while stood still, one fidget (with a gun, the ready stance's own; never while
        // aiming or just after shooting).
        bool fidgetable=f.Grounded && !f.Moving && !f.Turning && !f.Busy && moveClip<.1f && (f.IsLocomotion || f.IsCrouchLoco);
        if(f.FidgetNext<=0)f.FidgetNext=FidgetMin+(FidgetMax-FidgetMin)*f.Random;
        if(!fidgetable)f.FidgetTime=f.IsFidget?f.FidgetTime:0;
        else if((f.FidgetTime+=f.Dt)>=f.FidgetNext){
            int pick=(int)MathF.Floor(f.Random*997)%997;
            f.FidgetIndex=f.Crouched?pick%6:f.Armed?pick%2:2+pick%7;
            f.Triggers|=256;f.FidgetTime=0;f.FidgetNext=0;
        }
        f.Sprint=sprint;f.Airborne=f.AirTime>f.AirborneDelay;if(f.Jumped)f.Triggers|=32;
    }
}
