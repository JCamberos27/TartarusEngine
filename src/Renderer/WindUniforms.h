#pragma once
// The scene wind's per-frame uniforms (WindCommon.glsl), shared by the model, shadow, grass and impostor programs so
// every kind of vegetation reads one gust field: the same direction, clock and gust fronts.
#include "Material.h"
#include "Shader.h"

inline void SetWindFieldUniforms(const Shader& s) {
    const Material::WindField& w = Material::Wind();
    s.SetFloat("uWindTime", Material::WindTime());
    s.SetFloat("uPrevWindTime", Material::PrevWindTime());
    s.SetVec2("uWindDir", Material::WindDirection());
    s.SetFloat("uWindGlobal", w.Strength);
    s.SetFloat("uWindGustiness", w.Gustiness);
    s.SetFloat("uWindGustSpeed", w.GustSpeed);
    s.SetVec3("uWindCamPos", w.CameraPos);
}
