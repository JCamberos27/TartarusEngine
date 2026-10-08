using System.Numerics;

namespace Tartarus.Gameplay;

internal static unsafe class VitalsRules
{
    static void Remove(ref VitalsFrame f,int at)
    {
        for(int i=at;i+1<f.Indicators;i++) {
            f.SourceX[i]=f.SourceX[i+1];f.SourceY[i]=f.SourceY[i+1];f.SourceZ[i]=f.SourceZ[i+1];
            f.Age[i]=f.Age[i+1];f.Strength[i]=f.Strength[i+1];
        }
        f.Indicators--;
    }
    public static void Update(ref VitalsFrame f)
    {
        switch(f.Operation)
        {
            case 0:
                var reset=new VitalsFrame {MaxHealth=Math.Max(1,f.MaxHealth),RegenDelay=f.RegenDelay,RegenRate=f.RegenRate,
                    RespawnDelay=f.RespawnDelay,SpawnProtection=f.SpawnProtection,GodMode=f.GodMode,SinceHit=1e9f};
                reset.Health=reset.MaxHealth;f=reset;break;
            case 1:
                f.Taken=0;
                if(f.Dead!=0 || !(f.Amount>0))break;
                int index=-1;
                for(int i=0;i<f.Indicators;i++)
                    if(Vector3.Distance(new(f.SourceX[i],f.SourceY[i],f.SourceZ[i]),f.Source)<2) {index=i;break;}
                if(index>=0)f.Strength[index]=Math.Min(1,f.Strength[index]+.35f);
                else {
                    if(f.Indicators>=8) {
                        int oldest=0;for(int i=1;i<f.Indicators;i++)if(f.Age[i]>f.Age[oldest])oldest=i;
                        Remove(ref f,oldest);
                    }
                    index=f.Indicators++;f.Strength[index]=.6f;
                }
                f.SourceX[index]=f.Source.X;f.SourceY[index]=f.Source.Y;f.SourceZ[index]=f.Source.Z;f.Age[index]=0;
                if(f.GodMode!=0 || f.Protection>0)break;
                f.Taken=Math.Min(f.Amount,f.Health);f.Health-=f.Taken;f.DamageTaken+=f.Taken;f.SinceHit=0;
                f.HurtFlash=Math.Min(1,f.HurtFlash+.35f+.65f*f.Taken/f.MaxHealth*3);
                if(f.Health<=0) {f.Health=0;f.Dead=1;f.DiedThisTick=1;f.DeadTime=0;f.Deaths++;}
                break;
            case 2:
                float dt=Math.Max(0,f.Dt);f.JustDied=f.DiedThisTick;f.DiedThisTick=0;f.SinceHit+=dt;
                f.Protection=Math.Max(0,f.Protection-dt);
                if(f.Dead!=0)f.DeadTime+=dt;
                else if(f.SinceHit>=f.RegenDelay && f.RegenRate>0)f.Health=Math.Min(f.MaxHealth,f.Health+f.RegenRate*dt);
                for(int i=0;i<f.Indicators;) {f.Age[i]+=dt;if(f.Age[i]>=2)Remove(ref f,i);else i++;}
                f.HurtFlash=Math.Max(0,f.HurtFlash-dt*1.6f);f.Hitmarker=Math.Max(0,f.Hitmarker-dt*4);break;
            case 3:
                f.Dead=0;f.DeadTime=0;f.Health=f.MaxHealth;f.SinceHit=1e9f;f.Protection=f.SpawnProtection;
                f.Indicators=0;f.HurtFlash=0;break;
            case 4:f.Hitmarker=1;break;
            case 5:
                for(int i=0;i<f.Indicators;i++) {
                    var angle=new DamageFrame {Operation=5,Camera=f.Source,Yaw=f.CameraYaw,Source=new(f.SourceX[i],f.SourceY[i],f.SourceZ[i])};
                    DamageRules.Update(ref angle);f.ArcAngle[i]=angle.Result;
                    float life=1-f.Age[i]/2;f.ArcAlpha[i]=Math.Clamp(life*life*(.5f+.5f*f.Strength[i]),0,1);
                }
                break;
        }
        f.WantsRespawn=f.Dead!=0 && f.DeadTime>=f.RespawnDelay?1:0;
        f.Health01=f.MaxHealth>0?f.Health/f.MaxHealth:0;
        f.RespawnProgress=f.Dead==0?0:f.RespawnDelay>0?Math.Clamp(f.DeadTime/f.RespawnDelay,0,1):1;
        f.DeathFade=f.Dead!=0?Math.Clamp(f.DeadTime/1.2f,0,1):0;
        float t=Math.Clamp(f.DeadTime/.9f,0,1),ease=t*t*(3-2*t);
        f.DeathDrop=f.Dead!=0?-(f.EyeHeight-.35f)*ease:0;f.DeathRoll=f.Dead!=0?32*ease:0;
    }
}
