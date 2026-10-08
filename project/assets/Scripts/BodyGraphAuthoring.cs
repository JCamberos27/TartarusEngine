using System.Text.Json;
using System.Text.Json.Nodes;
using Tartarus;
namespace Tartarus.Gameplay;
public static class BodyGraphAuthoring
{
    static JsonNode Template()=>JsonNode.Parse(File.ReadAllText(new AssetReference("assets/Animations/Controllers/fps_body_locomotion.template.json").ResolvePath()))!;
    static IEnumerable<JsonObject> Clips(JsonNode node) {
        if(node is JsonObject obj){if(obj["clip"] is JsonValue v && v.TryGetValue<string>(out var clip) && clip.Length>0)yield return obj;foreach(var item in obj)if(item.Value!=null)foreach(var found in Clips(item.Value))yield return found;}
        else if(node is JsonArray array)foreach(var child in array)if(child!=null)foreach(var found in Clips(child))yield return found;
    }
    public static string[] Roles()=>Clips(Template()).Select(x=>x["clip"]!.GetValue<string>()).Distinct().ToArray();
    public static string Build(IReadOnlyDictionary<string,string> clips) {var graph=Template();foreach(var item in Clips(graph)){string role=item["clip"]!.GetValue<string>();item["clip"]=clips.TryGetValue(role,out var path)?path:"";}return graph.ToJsonString();}
    public static string Pick(string role,IEnumerable<string> files) {
        foreach(string file in files){string stem=Path.GetFileNameWithoutExtension(file.Replace('\\','/'));if(stem.StartsWith("AM_",StringComparison.OrdinalIgnoreCase))stem=stem[3..];if(stem.Equals(role,StringComparison.OrdinalIgnoreCase))return file;}return "";
    }
    internal static string Request(string operation,string json)=>operation switch {
        "body.roles"=>JsonSerializer.Serialize(Roles()),"body.graph"=>Build(JsonSerializer.Deserialize<Dictionary<string,string>>(json)!),
        "body.pick"=>PickRequest(json),_=>throw new ArgumentException(operation)
    };
    static string PickRequest(string json){using var d=JsonDocument.Parse(json);return JsonSerializer.Serialize(Pick(d.RootElement.GetProperty("role").GetString()!,d.RootElement.GetProperty("files").EnumerateArray().Select(x=>x.GetString()!)));}
}
