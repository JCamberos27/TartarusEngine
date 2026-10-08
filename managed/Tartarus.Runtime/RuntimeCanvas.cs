using System.Numerics;

namespace Tartarus;

public enum TextAlignment { Left,Center,Right }

/// <summary>A drawing scope supplied by the host for a runtime overlay. Handles expire when the callback returns.</summary>
public readonly struct RuntimeCanvas
{
    readonly uint token;
    public int Width { get; }
    public int Height { get; }
    public float Scale { get; }
    RuntimeCanvas(NativeRequest r) { token=r.Entity;Width=(int)r.A.X;Height=(int)r.A.Y;Scale=r.Value; }
    public static RuntimeCanvas? current {
        get { NativeRequest r=new() {Result=7};return Engine.Call(99,ref r)!=0?new RuntimeCanvas(r):null; }
    }
    NativeRequest Request(uint operation)=>new() {Result=7,Entity=token,Script=operation};
    static void Check(int result) {if(result==0)throw new InvalidOperationException("Runtime canvas scope has expired");}
    public float Measure(string text,float size) {
        var r=Request(1);r.Value=size;Check(Engine.TextCall(99,text,ref r));return r.Value;
    }
    public float LineHeight(float size) {
        var r=Request(2);r.Value=size;Check(Engine.Call(99,ref r));return r.Value;
    }
    public float Text(float x,float y,string text,float size,Vector4 color,TextAlignment alignment=TextAlignment.Left,bool shadow=true) {
        if(alignment<TextAlignment.Left || alignment>TextAlignment.Right)throw new ArgumentOutOfRangeException(nameof(alignment));
        var r=Request(3);r.A=new(x,y,size);r.B=new(color.X,color.Y,color.Z);r.Value=color.W;r.C=new((int)alignment,shadow?1:0,0);
        Check(Engine.TextCall(99,text,ref r));return r.Value;
    }
    public void Rect(float x,float y,float width,float height,Vector4 color) {
        var r=Request(4);r.A=new(x,y,width);r.C=new(height,0,0);r.B=new(color.X,color.Y,color.Z);r.Value=color.W;Check(Engine.Call(99,ref r));
    }
    public void Line(Vector2 from,Vector2 to,float thickness,Vector4 color) {
        var r=Request(5);r.A=new(from,thickness);r.C=new(to,0);r.B=new(color.X,color.Y,color.Z);r.Value=color.W;Check(Engine.Call(99,ref r));
    }
}
