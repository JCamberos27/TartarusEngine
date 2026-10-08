using System.Text.Json.Serialization;
using System.Text.Json;
using System.Runtime.InteropServices;

namespace Tartarus;

/// <summary>A serialized project asset identity; the host follows its GUID when a file moves.</summary>
public readonly record struct AssetReference(
    [property: JsonPropertyName("path")] string Path = "",
    [property: JsonPropertyName("pathGuid")] string Guid = "")
{
    public AssetReference() : this("", "") { }
    /// <summary>Follow the asset GUID and return an absolute path in the current project.</summary>
    public string ResolvePath() {
        NativeRequest request=new() {Result=6};
        if(Engine.TextCall(99,JsonSerializer.Serialize(this),ref request)==0) return Path;
        return JsonSerializer.Deserialize<string>(Marshal.PtrToStringUTF8(request.Text)!)??Path;
    }
}

[AttributeUsage(AttributeTargets.Field)]
public sealed class AssetPathAttribute(params string[] extensions) : Attribute
{
    public string[] Extensions { get; } = extensions;
}

[AttributeUsage(AttributeTargets.Field)] public sealed class ColorAttribute : Attribute { }
