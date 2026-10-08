using System.Numerics;

namespace Tartarus;

/// <summary>A validated handle to the native carry servo. The current backend supports one simultaneous carry constraint.</summary>
public readonly record struct CarryConstraint(uint Generation,GameObject Body)
{
    public bool IsValid {get {NativeRequest r=new() {Result=7,Entity=Body.Id,Script=Generation};return Engine.Call(58,ref r)!=0;}}
    public static bool TryCreate(GameObject body,out CarryConstraint constraint) {
        NativeRequest r=new() {Result=4,Entity=body.Id};bool ok=Engine.Call(58,ref r)!=0;constraint=ok?new(r.Script,body):default;return ok;
    }
    public bool SetTarget(Vector3 position,Vector3 velocity,Quaternion rotation) {
        NativeRequest r=new() {Result=5,Entity=Body.Id,Script=Generation,A=position,B=velocity,C=new(rotation.X,rotation.Y,rotation.Z),Value=rotation.W};return Engine.Call(58,ref r)!=0;
    }
    public bool Release(Vector3 velocity=default,bool launch=false,float backspin=0) {
        NativeRequest r=new() {Result=6,Entity=Body.Id,Script=Generation,A=velocity,C=new(launch?1:0,0,0),Value=backspin};return Engine.Call(58,ref r)!=0;
    }
}
public static partial class Physics
{
    public static bool TryGetBodyState(GameObject body,out Vector3 velocity,out bool kinematic) {
        NativeRequest r=new() {Entity=body.Id,Result=2};bool ok=Engine.Call(58,ref r)!=0;velocity=r.A;kinematic=r.Value!=0;return ok;
    }
    public static bool TryGetBodyRotation(GameObject body,out Quaternion rotation) {
        NativeRequest r=new() {Entity=body.Id,Result=3};bool ok=Engine.Call(58,ref r)!=0;rotation=ok?new(r.A.X,r.A.Y,r.A.Z,r.Value):Quaternion.Identity;return ok;
    }
    public static bool TryGetActorPosition(GameObject body,out Vector3 position) {
        NativeRequest r=new() {Entity=body.Id,Result=1};bool ok=Engine.Call(58,ref r)!=0;position=r.A;return ok;
    }
    public static void AddImpulseAtPosition(GameObject body,Vector3 impulse,Vector3 position) {
        NativeRequest r=new() {Entity=body.Id,Result=8,A=impulse,B=position};Engine.Call(58,ref r);
    }
}
