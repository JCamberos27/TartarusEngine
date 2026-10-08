using System.Numerics;
namespace Tartarus.Gameplay;
internal static class BodyLocomotion
{
    const float Rad=MathF.PI/180;
    static float Wrap(float a){a%=MathF.Tau;if(a>MathF.PI)a-=MathF.Tau;else if(a<=-MathF.PI)a+=MathF.Tau;return a;}
    static float Follow(float dt,float seconds)=>seconds>1e-4f?1-MathF.Exp(-dt/seconds):1;
    static Vector3 Local(Vector3 v,float yaw)=>new(-v.X*MathF.Cos(yaw)+v.Z*MathF.Sin(yaw),v.X*MathF.Sin(yaw)+v.Z*MathF.Cos(yaw),0);
    static float ClipSpeed(float speed,ref BodyMotionFrame f) {
        // Preserve the tuned walk/jog/sprint mapping in metres per second.
        speed=Math.Max(speed,0);float run=Math.Max(f.PlayerRunSpeed,.01f);
        if(speed<=run || f.PlayerSprintSpeed<=run+.001f)return speed*f.RunSpeed/run;
        return f.RunSpeed+(speed-run)*(f.ClipSprint-f.RunSpeed)/(f.PlayerSprintSpeed-run);
    }
    static Vector3 AsClip(Vector3 v,ref BodyMotionFrame f){var local=Local(v,f.Yaw);float s=local.Length();return s>1e-4f?local*(ClipSpeed(s,ref f)/s):Vector3.Zero;}
    internal static void Update(ref BodyMotionFrame f) {
        f.Triggers=0;f.SetTurnAngle=false;
        var moving=new Vector3(f.Velocity.X,0,f.Velocity.Z);float responsiveness=Math.Clamp(f.Responsiveness,0,1);
        var target=Vector3.Lerp(AsClip(f.WishVelocity,ref f),AsClip(moving,ref f),responsiveness);
        float movingSpeed=moving.Length();bool sprint=new Vector2(f.WishVelocity.X,f.WishVelocity.Z).Length()>f.PlayerRunSpeed*1.05f;
        float clip=ClipSpeed(movingSpeed,ref f);f.PlayRate=clip>.05f?1+(Math.Clamp(movingSpeed/clip,1,Math.Max(f.MaxPlayRate,1))-1)*Math.Min(clip/.5f,1)*(sprint?1:responsiveness):1;
        bool hold=f.StartStopClips && target.Length()<.01f && f.IdleTime<f.StopDebounce && f.Move.Length()>(f.Crouched?f.StopMinSpeedCrouched:f.StopMinSpeed);
        if(!hold)f.Move+=(target-f.Move)*Follow(f.Dt,f.ParamSmoothing);
        f.AirTime=f.Grounded?0:f.AirTime+f.Dt;
        bool still=f.Grounded && new Vector2(f.WishVelocity.X,f.WishVelocity.Z).Length()<.1f && f.Move.Length()<.2f;f.Still=still && f.TurnThreshold>0;
        float offset=Wrap(f.ViewYaw-f.Yaw);
        if(f.TurnThreshold<=0){f.Yaw=f.ViewYaw;f.Turning=false;}
        else if(f.Turning){
            f.TurnTime+=f.Dt;if(f.IsTurn || f.IsCrouchTurn){float step=f.RootYaw*Rad;f.Yaw+=step;f.TurnDone+=step;}
            offset=Wrap(f.ViewYaw-f.Yaw);bool clipDone=f.TurnTime>f.TurnMinTime && (!f.IsTurn || f.StateTime>=.97f);
            if(!still || Math.Abs(offset)<f.TurnEndAngle*Rad || clipDone || f.TurnTime>f.TurnTimeout)f.Turning=false;
        } else if(still){if(Math.Abs(offset)>f.TurnThreshold*Rad){f.Turning=true;f.TurnTime=f.TurnDone=0;f.TurnAngle=Math.Clamp(offset/Rad,-180,180);f.SetTurnAngle=true;}}
        else f.Yaw+=offset*Follow(f.Dt,f.TurnMoveEase);
        if(f.TurnThreshold>0){float lag=Wrap(f.ViewYaw-f.Yaw),max=Math.Max(f.TurnThreshold+f.TurnLagMargin,f.TurnLagFloor)*Rad;if(Math.Abs(lag)>max)f.Yaw=f.ViewYaw-MathF.CopySign(max,lag);}
        f.Yaw=Wrap(f.Yaw);f.Twist=Wrap(f.ViewYaw-f.Yaw);
        var wish=Local(f.WishVelocity,f.Yaw);float wishLen=wish.Length();f.Moving=wishLen>.1f;
        bool plain=f.StartStopClips && f.Grounded && (f.IsLocomotion || f.IsCrouchLoco) && !f.Turning;
        if(f.StartStopClips && f.Grounded && !f.Moving && f.Move.Length()<.4f && f.Crouched!=f.WasCrouched){if(f.Crouched && f.IsLocomotion)f.Triggers|=1;if(!f.Crouched && f.IsCrouchLoco)f.Triggers|=2;}
        f.WasCrouched=f.Crouched;
        if(f.Moving){var dir=wish/wishLen;if(plain && f.IdleTime>=f.StartIdleTime && f.Move.Length()<f.StartMaxMove){f.StartDir=dir;f.Triggers|=4;}f.LastDir=dir;f.LastSprint=sprint;f.IdleTime=0;f.MoveTime+=f.Dt;}
        else {float before=f.IdleTime;f.IdleTime+=f.Dt;bool enough=f.MoveTime>=(f.Crouched?f.StopMinRunTimeCrouched:f.StopMinRunTime);if(before<f.StopDebounce && f.IdleTime>=f.StopDebounce)f.MoveTime=0;if(plain && enough && before<f.StopDebounce && f.IdleTime>=f.StopDebounce && f.Move.Length()>(f.Crouched?f.StopMinSpeedCrouched:f.StopMinSpeed)){f.StopDir=f.LastDir;f.Triggers|=f.LastSprint && f.LastDir.Y>f.StopRunForward?16:8;}}
        f.Sprint=sprint;f.Airborne=f.AirTime>f.AirborneDelay;if(f.Jumped)f.Triggers|=32;
    }
}
