using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
namespace Tartarus.Gameplay;

internal static unsafe class AudioPolicy
{
    internal static string Folder(string gun)=>gun switch {"ak"=>"AKS74U","870"=>"Remington870",_=>gun};
    internal static string Gun(string path) {
        string lower=path.ToLowerInvariant();if(lower.Contains("aks74u"))return "ak";if(lower.Contains("remington870"))return "870";return Path.GetFileNameWithoutExtension(path).ToLowerInvariant();
    }
    static string Prefix(string key) {
        if(!key.StartsWith("snd.",StringComparison.Ordinal))return "";string rest=key[4..];
        bool foley=rest.StartsWith("foley.",StringComparison.Ordinal);if(foley)rest=rest[6..];int dot=rest.IndexOf('.');if(dot<=0 || dot+1>=rest.Length)return "";
        return foley?$"assets/Audio/Foley/{rest[..dot]}/{rest[(dot+1)..]}_":$"assets/Audio/Weapons/{Folder(rest[..dot])}/{rest[(dot+1)..]}_";
    }
    static object Parse(string key) {
        if(!key.StartsWith("snd.",StringComparison.Ordinal))return new {valid=false};string rest=key[4..];float lead=-1;int at=rest.IndexOf('@');
        if(at>=0){float.TryParse(rest[(at+1)..],NumberStyles.Float,CultureInfo.InvariantCulture,out lead);rest=rest[..at];}
        int dot=rest.IndexOf('.');if(dot<=0 || dot+1>=rest.Length)return new {valid=false};return new {valid=true,gun=rest[..dot],element=rest[(dot+1)..],lead};
    }
    static JsonObject Layer(string key,float near,float far,float nearWeight,float farWeight,float min,float max,int voices,float fade,int every=1,bool player2d=true)=>new() {
        ["key"]=key,["minDistance"]=min,["maxDistance"]=max,["maxVoices"]=voices,["stealFadeTime"]=fade,["pitchMin"]=1,["pitchMax"]=1,["volumeJitterDb"]=.5f,["every"]=every,["player2d"]=player2d,
        ["curve"]=new JsonObject {["nearDistance"]=near,["farDistance"]=far,["nearWeight"]=nearWeight,["farWeight"]=farWeight}
    };
    static JsonObject Default(string gun) {
        string k=$"snd.{gun}.fire_";var tail=Layer(k+"tail",30,120,1,.7f,6,140,3,.25f,2);
        var layers=new JsonObject { ["close"]=Layer(k+"close",18,43,1,0,3,90,6,.03f),["mech"]=Layer(k+"mech",10,30,1,0,3,45,6,.03f),["sub"]=Layer(k+"sub",25,70,1,0,6,120,6,.05f),["tail"]=tail,["far"]=Layer(k+"far",18,43,.1f,1,6,160,6,.1f,3,false)};
        foreach(string space in new[]{"outdoor_open","outdoor_urban","indoor_small","indoor_large"}){var t=(JsonObject)tail.DeepClone();t["key"]=k+"tail_"+space;layers["tail_"+space]=t;}
        return new() {["gun"]=gun,["tailMinInterval"]=0,["tailDuckPerVoice"]=.3f,["burstGap"]=.4f,["layers"]=layers,["aliases"]=new JsonObject {["ads_in"]="snd.foley.weapon.ads_in",["ads_out"]="snd.foley.weapon.ads_out",["equip"]="snd.foley.weapon.equip",["unequip"]="snd.foley.weapon.unequip"}};
    }
    internal static string Request(string operation,string json) {
        string text=JsonSerializer.Deserialize<string>(json)??"";
        return operation switch {
            "audio.gun"=>JsonSerializer.Serialize(Gun(text)),"audio.folder"=>JsonSerializer.Serialize(Folder(text)),"audio.prefix"=>JsonSerializer.Serialize(Prefix(text)),
            "audio.parse"=>JsonSerializer.Serialize(Parse(text)),"audio.default-profile"=>Default(text).ToJsonString(),
            "audio.profiles"=>JsonSerializer.Serialize(new[]{"ak","870"}),"audio.environment-profile"=>JsonSerializer.Serialize("ak"),_=>throw new ArgumentException(operation)
        };
    }
    internal static void Shot(ref AudioShotFrame f) {
        f.Pitch=f.PitchMin+(f.PitchMax-f.PitchMin)*f.Random;
        if(f.Now-f.LastShot>f.BurstGap)f.Count=0;int shot=f.Count++;f.LastShot=f.Now;
        for(int i=0;i<5;i++) {
            f.Play[i]=0;if(f.At2D && f.Player2D[i]==0 || f.Every[i]>1 && shot%f.Every[i]!=0)continue;
            float span=f.Far[i]-f.Near[i],t=span>1e-4f?Math.Clamp((f.Distance-f.Near[i])/span,0,1):f.Distance>=f.Far[i]?1:0;
            float weight=f.At2D?1:f.NearWeight[i]+(f.FarWeight[i]-f.NearWeight[i])*t;if(weight<.01f)continue;
            float gain=f.Volume*weight*(f.At2D?f.PlayerGain:1);
            if(i==4){if(f.Now-f.LastTail<f.TailMinInterval)continue;gain/=1+f.TailDuckPerVoice*f.TailVoices;f.LastTail=f.Now;}
            f.Play[i]=1;f.Gain[i]=gain;
        }
    }
}
