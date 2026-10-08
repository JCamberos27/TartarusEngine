using Tartarus;
namespace Tartarus.Gameplay;
internal static unsafe class WeaponLoadout
{
    internal static void Update(ref WeaponLoadoutFrame f){
        f.Commands=0;
        if(f.Operation==3){var ammo=(int*)f.SlotAmmo;for(int i=0;i<f.Count;i++)ammo[i]=-1;if(!f.Active)return;f.Ammo=f.Magazine;f.Chambered=true;f.CycleWait=0;f.Commands=4;return;}
        if(!f.Active)return;
        switch(f.Operation){
        case 0:
            if(f.Value<0 || f.Value>=f.Count)return;
            if(f.Value==f.Slot){f.Pending=-1;f.Commands=16;return;}f.Pending=f.Value;if(f.Equipped)f.Commands|=8;f.Equipped=false;f.Commands|=4;break;
        case 1:
            if(f.Count==0 || f.Value==0)return;int at=f.Pending>=0?f.Pending:f.Slot;f.Value=(at+(f.Value>0?1:-1)+f.Count)%f.Count;f.Commands=32;break;
        case 2:
            if(!f.Requested){f.Pending=-1;f.Burst=0;f.Commands|=2;}if(f.Requested!=f.Equipped)f.Commands|=8;f.Equipped=f.Requested;f.Commands|=4;break;
        case 4:if(f.Pending>=0 && f.Hidden && !f.InTransition)f.Commands=1;break;
        }
    }
}
