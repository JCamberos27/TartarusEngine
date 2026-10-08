using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Tartarus;
namespace Tartarus.Gameplay;
internal static unsafe class WardrobePolicy
{
    internal static string Name(string stem,bool hat=false) {
        string s=stem;foreach(string prefix in hat?new[]{"SKM_F_","SKM_","SM_"}:new[]{"SKM_F_","SKM_","SM_","Quantum_"})if(s.StartsWith(prefix,StringComparison.OrdinalIgnoreCase)){s=s[prefix.Length..];break;}
        if(hat)return s;string pretty=string.Join(' ',s.Split('_',StringSplitOptions.RemoveEmptyEntries).Select(w=>w.Equals("Tshirt",StringComparison.OrdinalIgnoreCase)?"T-Shirt":w));return pretty.Length>0?pretty:stem;
    }
    static int Layer(string slot)=>slot.ToLowerInvariant() switch {"shoes"=>2,"pants"=>3,"top"=>4,"outerwear"=>6,"collar"=>7,"bag" or "wrist l" or "wrist r"=>8,"hair" or "beard" or "glasses"=>9,"hat"=>10,_=>5};
    static bool Hides(string slot)=>slot.ToLowerInvariant() is not("hair" or "beard" or "glasses" or "wrist l" or "wrist r");
    internal static string Request(string operation,string json) {
        string s=JsonSerializer.Deserialize<string>(json)??"";
        return operation switch {"wardrobe.name"=>JsonSerializer.Serialize(Name(s)),"wardrobe.hat-name"=>JsonSerializer.Serialize(Name(s,true)),"wardrobe.layer"=>JsonSerializer.Serialize(Layer(s)),"wardrobe.hides"=>JsonSerializer.Serialize(Hides(s)),"wardrobe.gender"=>JsonSerializer.Serialize(s.Replace('\\','/').Split('/').Any(p=>p.Equals("Female",StringComparison.OrdinalIgnoreCase)) || Path.GetFileName(s.Replace('\\','/')).StartsWith("SKM_F_",StringComparison.OrdinalIgnoreCase)?1:0),_=>throw new ArgumentException(operation)};
    }
    internal sealed record Item(string Path,string Slot,int Sex,bool Variant);
    internal sealed record Style(string Name,float Weight,int Gender,Dictionary<string,float> Fill,float DefaultFill);
    internal sealed record Input(int Sex,string Race,Dictionary<string,string> Items,string[] Keep,string Style,string[] Races,string[] Slots,string[] Order,Item[] Catalog,Style[] Styles);
    static int Call(ref WardrobeRandomFrame f,int operation,ref NativeRequest request) {fixed(NativeRequest* ptr=&request)return ((delegate* unmanaged[Cdecl]<nint,int,NativeRequest*,int>)f.Services)(f.Context,operation,ptr);}
    static uint Raw(ref WardrobeRandomFrame f){NativeRequest r=default;Call(ref f,0,ref r);return r.Entity;}
    static float Random(ref WardrobeRandomFrame f,float max=1){NativeRequest r=new(){Value=max};Call(ref f,1,ref r);return r.Value;}
    static bool Conflict(ref WardrobeRandomFrame f,int a,int b){NativeRequest r=new(){Entity=(uint)a,Script=(uint)b};return Call(ref f,2,ref r)!=0;}
    static bool InStyle(ref WardrobeRandomFrame f,int item,int style){NativeRequest r=new(){Entity=(uint)item,Script=(uint)style};return Call(ref f,3,ref r)!=0;}
    internal static void Randomize(ref WardrobeRandomFrame f) {
        var input=JsonSerializer.Deserialize<Input>(Marshal.PtrToStringUTF8(f.Input)!)!;
        bool Kept(string name)=>input.Keep.Contains(name,StringComparer.OrdinalIgnoreCase);
        string race=input.Race;if(!Kept("Race") && input.Races.Length>0)race=input.Races[Raw(ref f)%(uint)input.Races.Length];
        int style=-1;if(input.Style.Length>0)style=Array.FindIndex(input.Styles,s=>s.Name.Equals(input.Style,StringComparison.OrdinalIgnoreCase));
        if(style<0){float total=input.Styles.Where(s=>s.Gender<0 || s.Gender==input.Sex).Sum(s=>Math.Max(s.Weight,0));float pick=Random(ref f,total);for(int i=0;i<input.Styles.Length;i++){var s=input.Styles[i];if(s.Gender>=0 && s.Gender!=input.Sex)continue;style=i;if((pick-=Math.Max(s.Weight,0))<=0)break;}}
        var items=new Dictionary<string,string>(input.Items);var worn=new List<int>();
        foreach(var pair in items)if(Kept(pair.Key)){int i=Array.FindIndex(input.Catalog,x=>x.Sex==input.Sex && x.Path.Equals(pair.Value.Replace('\\','/'),StringComparison.OrdinalIgnoreCase));if(i>=0)worn.Add(i);}
        var order=input.Order.ToList();foreach(string slot in input.Slots)if(!order.Contains(slot,StringComparer.OrdinalIgnoreCase))order.Add(slot);
        foreach(string slot in order){
            if(!input.Slots.Contains(slot) || Kept(slot))continue;items.Remove(slot);float chance=.3f;
            if(style>=0)chance=input.Styles[style].Fill.TryGetValue(slot,out var fill)?fill:input.Styles[style].DefaultFill;
            if(Random(ref f)>=chance)continue;var options=new List<int>();
            for(int i=0;i<input.Catalog.Length;i++){var item=input.Catalog[i];if(item.Sex!=input.Sex || item.Slot!=slot || item.Variant || style>=0 && !InStyle(ref f,i,style))continue;bool conflict=false;foreach(int o in worn)if(Conflict(ref f,i,o)){conflict=true;break;}if(!conflict)options.Add(i);}
            if(options.Count==0)continue;int chosen=options[(int)(Raw(ref f)%(uint)options.Count)];items[slot]=input.Catalog[chosen].Path;worn.Add(chosen);
        }
        byte[] output=Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new {Race=race,Items=items,Style=style>=0?input.Styles[style].Name:""})+'\0');
        fixed(byte* ptr=output){NativeRequest r=new(){Text=(nint)ptr};Call(ref f,4,ref r);}
    }
}
