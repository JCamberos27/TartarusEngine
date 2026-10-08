using System.Reflection;
using System.Text.Json;
namespace Tartarus.Gameplay;

internal static class NpcConfigData
{
    static readonly JsonSerializerOptions Options=new() {IncludeFields=true};
    public static string Resolve(string json) {
        using var document=JsonDocument.Parse(json);var root=document.RootElement;
        var fields=root.GetProperty("fields").GetRawText();
        object config;
        if(root.GetProperty("class").GetString()=="Tartarus.Gameplay.NpcDefinition") {
            var n=JsonSerializer.Deserialize<NpcDefinition>(fields,Options)??new();
            n.Weapon=(NpcLoadout)Math.Clamp((int)n.Weapon,0,2);n.Brain=(NpcBrainKind)Math.Clamp((int)n.Brain,0,1);
            n.Squad=Math.Clamp(n.Squad,0,16);n.Skill=Math.Clamp(n.Skill,0,1);n.OutfitSeed=Math.Clamp(n.OutfitSeed,0,100000);config=n;
        } else {
            var s=JsonSerializer.Deserialize<SquadDefinition>(fields,Options)??new();
            s.SquadSize=Math.Clamp(s.SquadSize,0,8);config=s;
        }
        return JsonSerializer.Serialize(config.GetType().GetFields(BindingFlags.Instance|BindingFlags.Public|BindingFlags.DeclaredOnly).ToDictionary(f=>f.Name,f=>f.GetValue(config)),Options);
    }
}
