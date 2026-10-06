using System.Numerics;
using Tartarus;

namespace Tartarus.Tests;

public sealed class ApiProbe : MonoBehaviour
{
    public override void Start()
    {
        Debug.Assert(gameObject.IsValid);
        var catalog = ComponentCatalog.GetAll();
        Debug.Assert(catalog.Length > 20 && catalog.All(c => c.Name.Length > 0));
        var child = Scene.Create("API Child", new(3, 2, 1));
        Debug.Assert(Scene.Find("API Child") == child);
        child.parent = gameObject;
        Debug.Assert(gameObject.children.Contains(child) && child.parent == gameObject);
        Debug.Assert(Vector3.Distance(child.transform.position, new(3, 2, 1)) < .001f);
        var source = child.AddNativeComponent("Audio Source");
        var field = catalog.Single(c => c.Name == "Audio Source").Fields.First(f => f.Kind == 2);
        source.Set(field.Key, .5f); Debug.Assert(source.Get<float>(field.Key) == .5f);
        Debug.Assert(Scene.FindObjectsWithComponent("Audio Source").Contains(child));
        child.RemoveNativeComponent("Audio Source"); Debug.Assert(!child.HasNativeComponent("Audio Source"));
        var camera=child.AddComponent<Camera>();camera.fieldOfView=250;
        Debug.Assert(child.GetComponent<Camera>()!.fieldOfView==179);
        var light=child.AddComponent<Light>();light.color=new(.2f,.4f,.6f);
        Debug.Assert(Vector3.Distance(light.color,new(.2f,.4f,.6f))<.001f);
        Debug.Assert(Vector3.Distance(child.transform.InverseTransformPoint(child.transform.TransformPoint(new(1,2,3))),new(1,2,3))<.001f);
        child.name = "API Renamed"; Debug.Assert(Scene.Find("API Child") == null && Scene.Find(child.name) == child);
        var clock = Time.GetSnapshot(); Debug.Assert(double.IsFinite(clock.Realtime));
        Debug.Assert(Mathf.DeltaAngle(350, 10) == 20 && Mathf.Lerp(0, 10, 2) == 10);
        GameObject.Destroy(child); Debug.Assert(!child.IsValid);
        GetComponent<Animator>()!.SetFloat("ApiPassed", 1);
    }
}
