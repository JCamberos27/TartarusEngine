using System.Runtime.CompilerServices;

namespace Tartarus.Gameplay;

// The native adapter supplies state/services; all operation meanings belong to this project.
internal static unsafe class GameIntegration
{
    public static bool Invoke(string operation,nint data,int size)
    {
        if(data==0)return false;
        switch(operation)
        {
            case "body.motion" when size==Unsafe.SizeOf<BodyMotionFrame>():BodyLocomotion.Update(ref *(BodyMotionFrame*)data);return true;
            case "weapon.loadout" when size==Unsafe.SizeOf<WeaponLoadoutFrame>():WeaponLoadout.Update(ref *(WeaponLoadoutFrame*)data);return true;
            default:return false;
        }
    }
    public static string Request(string operation,string json) => operation switch
    {
        "body.roles" or "body.graph" or "body.pick" => BodyGraphAuthoring.Request(operation,json),
        _ => throw new ArgumentException("Unknown project request: "+operation)
    };
}
