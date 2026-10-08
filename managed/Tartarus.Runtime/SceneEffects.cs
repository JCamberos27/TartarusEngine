using System.Numerics;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Tartarus;

public readonly partial record struct GameObject
{
    public string tag {
        get { NativeRequest r=new() {Entity=Id,Result=1};
            if(Engine.Call(99,ref r)==0) throw new InvalidOperationException("Invalid object");
            return JsonSerializer.Deserialize<string>(Marshal.PtrToStringUTF8(r.Text)!)!; }
    }
    /// <summary>Set emission on an assigned material slot. Shared materials retain shared semantics.</summary>
    public bool SetMaterialEmission(float strength,uint materialSlot=0) {
        NativeRequest r=new() {Entity=Id,Result=2,Script=materialSlot,Value=strength};return Engine.Call(99,ref r)!=0;
    }
}
public static partial class Physics
{
    public static GameObject? constrainedObject {
        get { NativeRequest r=new() {Result=4};return Engine.Call(99,ref r)!=0 && r.Entity!=uint.MaxValue?new(r.Entity):null; }
    }
    public static ulong stepSequence {
        get { NativeRequest r=new() {Result=5};if(Engine.Call(99,ref r)==0)return 0;
            return JsonSerializer.Deserialize<ulong>(Marshal.PtrToStringUTF8(r.Text)!); }
    }
    public static bool TryGetBodyPosition(GameObject body,out Vector3 position) {
        NativeRequest r=new() {Entity=body.Id,Result=3};bool ok=Engine.Call(99,ref r)!=0;position=r.A;return ok;
    }
}
