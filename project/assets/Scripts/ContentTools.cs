using System.Text.Json;
using Tartarus;
namespace Tartarus.Gameplay;
// Pack selection and output locations belong to the project. Native code only converts the supplied data.
internal static class ContentTools {
    internal static string PathFor(string name)=>name switch {"blood"=>"assets/Effects/Blood","knife"=>"assets/Effects/Knife","body"=>new SquadDefinition().BodyPrefab.Path,_=>throw new ArgumentException(name)};
    internal static string Request(string operation,string json) {
        if(operation=="content.path")return JsonSerializer.Serialize(PathFor(JsonSerializer.Deserialize<string>(json)!));
        string source=operation switch {"content.blood"=>"BloodImport","content.knife"=>"KnifeImport",_=>throw new ArgumentException(operation)};
        return File.ReadAllText(new AssetReference("assets/Editor/Content/"+source+".json").ResolvePath());
    }
}
