#pragma once
class PropertyRows;
struct WeaponRecoilSettings;
namespace RecoilAssetEditor {
// forcedTab is used only by the isolated visual capture harness; normal inspectors use -1.
void Draw(PropertyRows&,WeaponRecoilSettings&,float rpm,int forcedTab=-1);
}
