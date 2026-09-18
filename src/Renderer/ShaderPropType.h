#pragma once

// The value type of a .shader Properties{} entry. Its own header so Material.h can hold typed
// custom-property values without pulling in ShaderAsset.
enum class ShaderPropType { Float, Color, Texture2D, Bool, Vec2, Vec3, Vec4, Int };
