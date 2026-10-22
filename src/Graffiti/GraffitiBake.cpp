#include "GraffitiCompositor.h"
#include "AtomicFile.h"
#include "DecalAsset.h"
#include "Log.h"
#include "ProjectPaths.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace Graffiti {

bool WriteBake(const Piece& piece, const BakeFiles& files) {
    if (files.Albedo.empty()) return false;
    const struct { const char* Map; const std::string* Bytes; } maps[] = {
        {"Albedo", &files.Albedo}, {"Normal", &files.Normal}, {"Surface", &files.Surface}, {"Aux", &files.Aux}};
    for (const auto& m : maps)
        if (!AtomicFile::WriteBytes(fs::u8path(piece.MapPath(m.Map)), *m.Bytes, /*binary=*/true)) {
            Log::Error("Graffiti: couldn't write '" + piece.MapPath(m.Map) + "'.", LogContext::Asset(piece.Path));
            return false;
        }

    const std::string decalPath = piece.DecalPath();
    std::error_code ec;
    const bool existed = fs::exists(fs::u8path(decalPath), ec);
    DecalAsset d = existed ? *DecalAsset::Load(decalPath) : DecalAsset{};
    d.Path = decalPath;
    if (!existed || d.Missing) {
        d = DecalAsset{};
        d.Path = decalPath;
        d.DefaultSize = glm::vec3(piece.Metres, 0.25f);
        d.EdgeFade = 0.04f;
        // The piece's own height already thins its overspray (ShowThrough); a height blend on top
        // would take the soft edges away again, so the paint lies on the wall as designed.
        d.HeightBlend = 0.0f;
        d.Fade = piece.Fade;
        d.Bleach = piece.Bleach;
        d.Peel = piece.Peel;
        d.Buff = piece.Buff;
        d.SceneGrading = false; // the scene's Age / Wear would peel and chip a fresh piece apart
        d.Category = "Graffiti/Pieces";
        d.Tags = "graffiti, piece";
    }
    d.Missing = false;
    d.AlbedoPath = ProjectPaths::Relativize(piece.MapPath("Albedo"));
    d.NormalPath = ProjectPaths::Relativize(piece.MapPath("Normal"));
    d.SurfacePath = ProjectPaths::Relativize(piece.MapPath("Surface"));
    d.AuxPath = ProjectPaths::Relativize(piece.MapPath("Aux"));
    d.LumaMask = d.InvertMask = false;
    d.Tier = std::max(piece.Width, piece.Height);
    d.AtlasCols = d.AtlasRows = 1;
    const bool metal = std::any_of(piece.Layers.begin(), piece.Layers.end(), [](const Layer& l) { return l.Visible && l.Metal > 0.0f; });
    const bool glow = piece.GlowStrength > 0.0f;
    d.AlbedoOp = 1.0f;
    d.NormalOp = 1.0f;
    d.RoughnessOp = 1.0f;
    d.MetalOp = metal ? 1.0f : 0.0f;
    d.EmissiveOp = glow ? 1.0f : 0.0f;
    d.BaseColor = glm::vec3(1.0f);
    d.Roughness = piece.Roughness;
    d.Emissive = glow ? SrgbToLinear(piece.GlowColor) * piece.GlowStrength : glm::vec3(0.0f);
    d.Paint = true;
    d.PaintRoughness = piece.Roughness;
    d.PaintMetal = metal ? 1.0f : 0.0f;
    d.Relief = piece.Relief;
    d.UVPaint = piece.UV;
    if (!d.Save()) {
        Log::Error("Graffiti: couldn't write '" + decalPath + "'.", LogContext::Asset(piece.Path));
        return false;
    }
    return true;
}

std::shared_ptr<Piece> SnapshotForBake(const Piece& piece) {
    auto s = std::make_shared<Piece>();
    s->Path = piece.Path;
    s->Name = piece.Name;
    s->Width = piece.Width;
    s->Height = piece.Height;
    s->Metres = piece.Metres;
    s->Seed = piece.Seed;
    s->Roughness = piece.Roughness;
    s->Relief = piece.Relief;
    s->ShowThrough = piece.ShowThrough;
    s->Drips = piece.Drips;
    s->GlowColor = piece.GlowColor;
    s->GlowStrength = piece.GlowStrength;
    s->UV = piece.UV;
    s->Fade = piece.Fade;
    s->Bleach = piece.Bleach;
    s->Peel = piece.Peel;
    s->Buff = piece.Buff;
    if (std::any_of(piece.Layers.begin(), piece.Layers.end(), [](const Layer& l) { return l.Visible && l.Metal > 0.0f; })) {
        Layer metal;
        metal.Visible = true;
        metal.Metal = 1.0f;
        s->Layers.push_back(metal);
    }
    return s;
}

} // namespace Graffiti
