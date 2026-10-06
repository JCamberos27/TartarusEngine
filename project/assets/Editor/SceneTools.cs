using System.Numerics;
using Tartarus;
using Tartarus.Editor;

namespace Tartarus.EditorTools;

/// <summary>Example native dockable window authored entirely in C#. Each command records a single undo step.</summary>
public sealed class SceneTools : EditorWindow
{
    public override string Title => "Scene Tools";
    [MenuItem("Scene/Open Scene Tools")]
    public static void OpenTools() => GetWindow<SceneTools>();

    public override void OnGUI()
    {
        var selected = Selection.activeGameObject;
        EditorGUILayout.Label(selected.HasValue ? "Selected: " + selected.Value.name : "Select an object in the Hierarchy.");
        if (selected.HasValue && EditorGUILayout.Button("Reset local position")) {
            Undo.RecordScene("Reset position from C#");
            selected.Value.transform.localPosition = Vector3.Zero;
        }
        if (EditorGUILayout.Button("Create empty object")) {
            Undo.RecordScene("Create object from C#");
            Selection.activeGameObject = Scene.Create("C# Object");
        }
        if (EditorGUILayout.Button("Log scripting API catalog")) {
            foreach (var type in ComponentCatalog.GetAll()) Debug.Log($"{type.Category}/{type.Name}: {type.Fields.Length} fields");
        }
    }
}
