using System.Runtime.InteropServices;
using System.Numerics;
using Tartarus;
namespace Tartarus.Gameplay;
internal static unsafe class DevTools
{
    static int Service(ref DevFrame f,int operation,int index,Vector3 feet=default){NpcServiceFrame r=new(){Operation=operation,Index=index,A=new(0,.2f,-1),B=feet+new Vector3(0,1.2f,0),Value=20};return ((delegate* unmanaged[Cdecl]<nint,NpcServiceFrame*,int>)f.Services)(f.Context,&r);}
    static int Kill(ref DevFrame f){int count=0;var members=(NpcMateFrame*)f.Members;for(int i=0;i<f.Count;i++)if(!members[i].Dead)count+=Service(ref f,0,members[i].Index,members[i].Feet);return count;}
    static int Spawn(ref DevFrame f){if(f.Spawns<=0)return 0;int made=0;for(int i=0;f.Alive+made<f.Wanted && i<f.Wanted*2;i++)made+=Service(ref f,1,i%f.Spawns);return made;}
    internal static void Update(ref DevFrame f){
        switch(f.Operation){
        case 0:f.Open=f.InfiniteAmmo=f.Invisible=f.AiOverlay=f.God=f.Frozen=f.HoldFire=false;break;
        case 1:if(Input.GetKeyDown(296))f.Open=!f.Open;if(Input.GetKeyDown(297))f.God=!f.God;if(Input.GetKeyDown(298))f.AiOverlay=!f.AiOverlay;break;
        case 3:f.Result=Kill(ref f);break;
        case 4:f.Result=Spawn(ref f);break;
        case 2:
            if(!f.Open || RuntimeGui.current is not {} gui)return;bool open=f.Open;bool visible=gui.BeginWindow("Dev (F7)",ref open);f.Open=open;
            try {if(!visible)return;
                gui.Separator("Player");f.God=gui.Checkbox("God mode (F8)",f.God);f.InfiniteAmmo=gui.Checkbox("Infinite ammo",f.InfiniteAmmo);f.Invisible=gui.Checkbox("Invisible to AI",f.Invisible);
                gui.Separator("Squad");f.Frozen=gui.Checkbox("Freeze AI",f.Frozen);f.HoldFire=gui.Checkbox("AI holds fire",f.HoldFire);f.Difficulty=gui.Slider("Difficulty",f.Difficulty,.25f,3);
                if(gui.Button("Kill all"))Kill(ref f);gui.SameLine();if(gui.Button("Respawn squad")){Kill(ref f);f.Alive=0;Spawn(ref f);}
                gui.Label($"{f.Alive} alive, {f.Known} in combat");gui.Separator("World");f.TimeScale=gui.Slider("Time scale",f.TimeScale,.05f,2);gui.SameLine();if(gui.Button("1x"))f.TimeScale=1;
                f.AiOverlay=gui.Checkbox("AI debug overlay (F9)",f.AiOverlay);if(f.HaveBloodLab)f.BloodLab=gui.Checkbox("Blood Lab",f.BloodLab);gui.Label("F7 panel   F8 god mode   F9 AI overlay",true);
            }finally {gui.EndWindow();}break;
        }
    }
}
