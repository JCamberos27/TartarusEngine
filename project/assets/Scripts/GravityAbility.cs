using System.Numerics;
using Tartarus;

namespace Tartarus.Gameplay;

// This project's input, aim assistance, charge and launch policy. Physics owns the servo/sweeps.
internal static class GravityAbility
{
    const uint None=uint.MaxValue;
    static bool Dynamic(GameObject body) => body.Id!=None && Physics.TryGetBodyState(body,out _,out bool kinematic) && !kinematic;
    internal static float AssistReach(float assist,float hitDistance) => hitDistance<0?assist:Math.Min(assist,hitDistance+.5f);
    static GameObject FindTarget(in GravityFrame f) {
        float reach=f.AssistRange;
        if(Physics.Raycast(f.Eye,f.Forward,out var hit,f.GrabRange)) {
            if(Dynamic(hit.Object))return hit.Object;
            reach=AssistReach(f.AssistRange,hit.Distance);
        }
        GameObject best=new(None);float cone=f.AssistConeDeg*MathF.PI/180,bestCos=MathF.Cos(cone);
        for(float t=.5f;t<reach;) {
            float radius=Math.Clamp(t*MathF.Tan(cone),.3f,2.5f);
            foreach(var candidate in Physics.OverlapSphere(f.Eye+f.Forward*t,radius,32)) {
                if(candidate==best || !Dynamic(candidate) || !Physics.TryGetActorPosition(candidate,out var p))continue;
                Vector3 to=p-f.Eye;float distance=to.Length();if(distance<.001f || distance>reach+1)continue;
                float cosine=Vector3.Dot(to/distance,f.Forward);if(cosine<=bestCos)continue;
                if(!Physics.Raycast(f.Eye,to/distance,out var los,distance+.5f) || los.Object!=candidate)continue;
                best=candidate;bestCos=cosine;
            }
            t+=radius;
        }
        return best;
    }
    internal static void Update(ref GravityFrame f) {
        if(f.Operation==2) {f.HoldDistance=AssistReach(f.AssistRange,f.Scroll);return;}
        if(f.Operation==0) {f.LeftPrev=f.RightPrev=f.Charging=f.HaveHoldPoint=0;f.Charge=0;f.PredictSpeed=0;if(f.RotationW==0) {f.RotationW=1;f.HoldDistance=3;}return;}
        var constraint=new CarryConstraint(unchecked((uint)f.Handle),new(unchecked((uint)f.Held)));
        f.PredictSpeed=0;
        if(f.Right!=0 && f.RightPrev==0 && !constraint.IsValid) {
            GameObject target=FindTarget(f);
            if(target.Id!=None && Physics.TryGetActorPosition(target,out var p) && CarryConstraint.TryCreate(target,out constraint)) {
                f.Handle=unchecked((int)constraint.Generation);f.Held=unchecked((int)target.Id);
                f.HoldDistance=Math.Clamp(Vector3.Distance(p,f.Eye),1.5f,8);
                Physics.TryGetBodyRotation(target,out var q);f.Rotation=new(q.X,q.Y,q.Z);f.RotationW=q.W;
                f.HoldYaw=f.Yaw;f.HaveHoldPoint=f.Charging=0;
            }
        }
        if(constraint.IsValid) {
            float delta=MathF.IEEERemainder(f.Yaw-f.HoldYaw,MathF.Tau);f.HoldYaw=f.Yaw;
            Quaternion rotation=Quaternion.CreateFromAxisAngle(Vector3.UnitY,-delta)*new Quaternion(f.Rotation,f.RotationW);
            if(f.Scroll!=0) {
                float turn=f.ScrollTurnDeg*MathF.PI/180*f.Scroll;
                if(f.Alt!=0)rotation=Quaternion.CreateFromAxisAngle(Vector3.UnitY,turn)*rotation;
                else if(f.Control!=0)rotation=Quaternion.CreateFromAxisAngle(f.CameraRight,turn)*rotation;
                else f.HoldDistance=Math.Clamp(f.HoldDistance+f.Scroll*.6f,1.5f,8);
            }
            rotation=Quaternion.Normalize(rotation);f.Rotation=new(rotation.X,rotation.Y,rotation.Z);f.RotationW=rotation.W;
            Vector3 point=f.Eye+f.Forward*f.HoldDistance;
            if(f.HaveHoldPoint==0)f.HoldVelocity=Vector3.Zero;
            else if(f.Dt>.0001f) {
                Vector3 raw=(point-f.PrevHoldPoint)/f.Dt;float speed=raw.Length();if(speed>40)raw*=40/speed;
                f.HoldVelocity+=(raw-f.HoldVelocity)*(1-MathF.Exp(-f.Dt*25));
            }
            f.PrevHoldPoint=point;f.HaveHoldPoint=1;constraint.SetTarget(point,f.HoldVelocity,rotation);
            if(f.Left!=0 && f.LeftPrev==0) {f.Charging=1;f.Charge=0;}
            if(f.Charging!=0 && f.Left!=0)f.Charge=Math.Min(1,f.Charge+f.Dt/Math.Max(f.ChargeTime,.01f));
            float launch=f.MinThrowSpeed+(f.MaxThrowSpeed-f.MinThrowSpeed)*f.Charge;
            if(f.Charging!=0 && f.Left==0) {
                constraint.Release(f.Forward*launch,true,f.Backspin*MathF.Tau);f.Charging=f.HaveHoldPoint=0;f.Charge=0;
            } else if(f.Right==0 && f.RightPrev!=0) {
                constraint.Release();f.Charging=f.HaveHoldPoint=0;f.Charge=0;
            } else if(f.Charging!=0)f.PredictSpeed=launch;
        } else {
            f.Charging=f.HaveHoldPoint=0;f.Charge=0;
            if(f.Left!=0 && f.LeftPrev==0 && Physics.Raycast(f.Eye,f.Forward,out var hit,100) && hit.Object.Id!=None)
                Physics.AddImpulseAtPosition(hit.Object,f.Forward*6+Vector3.UnitY*2,hit.Point);
        }
        f.LeftPrev=f.Left;f.RightPrev=f.Right;
    }
}
