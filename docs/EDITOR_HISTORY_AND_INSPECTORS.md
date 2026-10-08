# Editor history and C# Inspectors

Ctrl+Z and Ctrl+Y use one chronological editor history; Ctrl+Shift+Z also redoes. Scene edits, component changes, selection, committed curve edits, asset writes, imports, asset duplication/rename/deletion, script creation, project settings and saved editor preferences share this history. A continuous drag produces one entry. Unchanged writes do not create entries or discard redo. The history holds the latest 100 actions in this editor session.

File history records the original bytes before the first write in an action, including binary files and GUID sidecars. Undoing creation removes the exact file; undoing deletion restores it. Rename restores both original and destination paths. Redo restores the final bytes. File restoration rolls back if a write fails. Imported/model-derived caches and compiler outputs are regenerated rather than journaled.

History caches the last committed authored scene. Camera movement, scrolling, docking, search and ordinary selection do not serialize it. Scene controls request a transaction through PushUndo/StageUndo (managed commands use Undo.RecordScene/RecordObject); commits refresh the cache, and scene replacement, history restoration and external asset changes refresh its context. File-only actions reuse the history anchor. The profiler's **Undo Scene Snapshot** scope identifies initialization and scene-edit commit costs. See [PERFORMANCE.md](PERFORMANCE.md) for the interaction regression check.

Text fields retain their normal character undo while typing; committed authored values join global history. Gameplay owns its input while the game viewport has captured the mouse. Release capture to use editor shortcuts. Asset undo remains available during Play, and its history survives Stop. Scene undo requires stopping Play because restoring the registry beneath live physics actors is unsafe. Runtime simulation is not an authored edit. Loading another scene or entering/exiting prefab editing creates a new scene history context. External IDE/Explorer changes and operating-system actions are outside this history.

## Recoil editing

Select a `.recoil` asset to tune its scalar settings and view read-only X/Y/Z curve thumbnails. Double-click a thumbnail to open a dedicated dockable curve window. Each window names its asset, group and axis. Drag keys/handles, change numeric values, or use presets there. Edits save on commit and use global undo. Multiple axes can be open together. A changed file is reloaded by the other windows. Existing asset formats and interpolation are preserved.

## Organized gameplay Inspectors

`project/assets/Editor/GameplayInspectors.cs` supplies custom C# Inspectors for First Person Body, First Person Controller, Weapon Definition and Bob. Every other attachable C# script gets the shared searchable fallback. The FPS body has Overview, Rig & Camera, Arms & Aim, Locomotion, Turning & Transitions, Foot IK, NPC and Advanced pages. Search includes all pages, labels, groups and tooltips. Native fields retain their C++ asset pickers, bounds, conditional visibility and authoring helpers.

These editors compile into `Tartarus.Editor.dll`, independently of gameplay, and are excluded from game exports. The native editor, world and ImGui ownership remain C++. Source-save compilation/reload is the same workflow used by editor windows.

Weapon Definition targets `Tartarus.Gameplay.WeaponDefinition`, an ordinary project script in
`assets/Scripts/WeaponDefinition.cs`. Its schema/defaults and Inspector belong to C#; it has no
native component registration. Animation Set uses an `AssetReference` with a GUID-preserving
picker, and light tint uses `[Color]`. Older native data migrates to a C# slot on read.

## Writing a custom Inspector

Place this in `assets/Editor`, using a fully qualified gameplay class or an exact reflected native component name:

```csharp
using Tartarus.Editor;

[CustomEditor("Tartarus.Gameplay.Bob")]
public sealed class MyBobEditor : Tartarus.Editor.Editor
{
    public override void OnInspectorGUI()
    {
        serializedObject.Update();
        EditorGUILayout.HelpBox("Author the movement settings below.");
        var height = serializedObject.FindProperty("Height");
        if (height != null) EditorGUILayout.PropertyField(height);
        serializedObject.ApplyModifiedProperties();
    }
}
```

Replace the existing Bob Inspector before adding this example: duplicate registrations are rejected. `[CustomEditor("*")]` supplies one fallback for attachable scripts. Inspectors receive `target` (a GameObject) and a fresh `serializedObject` each draw. `FindProperty` accepts the serialized name or display label; `Properties` enumerates every visible supported field. Property metadata includes Name, DisplayName, Group, Tooltip, Kind, Min/Max, EnumLabels/EnumValues and IsVisible. `GetValue<T>()` and `SetValue<T>()` read/write typed values. Script overrides are applied at draw end; native writes go through the reflected component API. This is the engine's implemented Unity-style API, not Unity binary compatibility.

On gameplay fields, `[Header("Movement")]` groups that field and following fields until another header. `[Range(min,max)]`, `[Tooltip("...")]`, `[SerializeField]` and `[HideInInspector]` control authoring. Grouping does not change serialized keys or existing values.

`EditorGUILayout` adds PropertyField, Toolbar, Popup, Foldout/EndFoldout, HelpBox and Tooltip to the existing native widgets. Always balance an opened foldout with EndFoldout, preferably in try/finally. The host also unwinds open groups after an exception and falls back to the native/default Inspector when the custom editor faults.

For commands, call `Undo.RecordScene(label)` or `Undo.RecordObject(target,label)` before scene mutation. The latter captures the authored scene transaction, rather than only that object. Call `Undo.RecordAsset(path)` before using ordinary .NET file APIs to overwrite/delete an asset; native AtomicFile writes are recorded automatically. `Undo.PerformUndo()` and `Undo.PerformRedo()` queue restoration until all panels finish drawing so their live references stay valid. Keep editor mutations on the main thread. Build/compiler/cache outputs should not be authored as assets.

Right-clicking or left-clicking the game viewport during Play engages game input identically.
