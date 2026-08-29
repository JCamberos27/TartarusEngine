#pragma once
#include <functional>
#include "Texture.h" // TextureImportSettings
#include "Model.h"   // ModelImportSettings

// Draws the Unity-AssetImporter-style settings block shown in the Inspector when a texture or
// model is selected in the Asset Browser (see EditorLayer::DrawAssetImportInspector). Pure UI —
// these functions only ever mutate the `settings` struct handed to them and flip `isDirty`;
// nothing here touches AssetLibrary, a Texture, or a Model directly, so the same functions could
// just as well back a "reimport a batch of assets with shared settings" dialog later.
class AssetImporterInspector {
public:
    static void DrawTextureSettings(TextureImportSettings& settings, bool& isDirty);
    static void DrawModelSettings(ModelImportSettings& settings, bool& isDirty);

    // isDirty gates both buttons the way Unity's AssetImporterEditor does — Apply/Revert only
    // make sense once something has actually changed from what's on disk. onApply/onRevert are
    // only invoked on the frame their button is clicked.
    static void DrawApplyRevertFooter(bool isDirty, const std::function<void()>& onApply,
        const std::function<void()>& onRevert);
};
