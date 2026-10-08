namespace Tartarus;
/// <summary>Immediate runtime widgets, available only during a host-supplied UI scope.</summary>
public readonly struct RuntimeGui
{
    readonly uint token;
    RuntimeGui(uint value){token=value;}
    public static RuntimeGui? current {get{NativeRequest r=new(){Result=8};return Engine.Call(99,ref r)!=0?new RuntimeGui(r.Entity):null;}}
    NativeRequest Request(uint operation)=>new(){Result=8,Entity=token,Script=operation};
    void Call(string text,ref NativeRequest r){if(Engine.TextCall(99,text,ref r)==0)throw new InvalidOperationException("Runtime UI scope has expired or widgets are unbalanced");}
    void Call(ref NativeRequest r){if(Engine.Call(99,ref r)==0)throw new InvalidOperationException("Runtime UI scope has expired or widgets are unbalanced");}
    public bool BeginWindow(string title,ref bool open,float width=320,float x=24,float y=80){var r=Request(1);r.Value=open?1:0;r.A=new(width,x,y);Call(title,ref r);open=r.Value!=0;return r.C.X!=0;}
    public void EndWindow(){var r=Request(2);Call(ref r);}
    public void Separator(string text){var r=Request(3);Call(text,ref r);}
    public bool Checkbox(string text,bool value){var r=Request(4);r.Value=value?1:0;Call(text,ref r);return r.Value!=0;}
    public float Slider(string text,float value,float min,float max){var r=Request(5);r.Value=value;r.A=new(min,max,0);Call(text,ref r);return r.Value;}
    public bool Button(string text){var r=Request(6);Call(text,ref r);return r.Value!=0;}
    public void SameLine(){var r=Request(7);Call(ref r);}
    public void Label(string text,bool disabled=false){var r=Request(8);r.Value=disabled?1:0;Call(text,ref r);}
}
