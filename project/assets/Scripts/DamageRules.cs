using System.Numerics;

namespace Tartarus.Gameplay;

internal static class DamageRules
{
    internal static float Falloff(float distance,float start,float end,float min)
    {
        min=Math.Clamp(min,0,1);
        return !(distance>start)?1:!(end>start) || distance>=end?min:1+(min-1)*(distance-start)/(end-start);
    }
    internal static int BoneRegion(string? bone)
    {
        if(bone==null)return 1;
        bool Has(string text)=>bone.Contains(text,StringComparison.Ordinal);
        if(bone=="head" || Has("jaw") || Has("eye"))return 0;
        if(new[]{"upperarm","lowerarm","hand","index","middle","ring","pinky","thumb","twist"}.Any(Has))
            return Has("thigh") || Has("calf")?3:2;
        return new[]{"thigh","calf","foot","ball","toe"}.Any(Has)?3:1;
    }
    internal static float IndicatorAngle(Vector3 camera,float yaw,Vector3 source) {
        var f=new DamageFrame {Operation=5,Camera=camera,Yaw=yaw,Source=source};Update(ref f);return f.Result;
    }
    public static void Update(ref DamageFrame f)
    {
        switch(f.Operation)
        {
            case 0:f.Result=Falloff(f.Distance,f.Start,f.End,f.MinScale);break;
            case 1:
                f.Result=Math.Max(0,f.Damage*(f.Zone==0?f.HeadMultiplier:f.Zone==2?f.LimbMultiplier:1)*Falloff(f.Distance,f.Start,f.End,f.MinScale));break;
            case 2:
                float height=f.Height>0?(f.HitY-f.FootY)/f.Height:0;
                f.Zone=f.Height<=0?1:height>.86f?0:height<.5f?2:1;break;
            case 3:f.Zone=f.Region==0?0:f.Region==2 || f.Region==3?2:1;break;
            case 4:f.Region=f.Part==2?0:f.Part>=3 && f.Part<=6?2:f.Part>=7 && f.Part<=10?3:1;break;
            case 5:
                float yaw=f.Yaw*MathF.PI/180;Vector2 forward=new(MathF.Cos(yaw),MathF.Sin(yaw)),right=new(-forward.Y,forward.X);
                Vector2 delta=new(f.Source.X-f.Camera.X,f.Source.Z-f.Camera.Z);
                f.Result=delta.LengthSquared()<1e-8f?0:MathF.Atan2(Vector2.Dot(delta,right),Vector2.Dot(delta,forward));break;
        }
    }
}
