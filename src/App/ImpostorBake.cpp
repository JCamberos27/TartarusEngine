#include "App/ImpostorBake.h"

#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "Components.h"
#include "Core/Paths.h"
#include "GLStateCache.h"
#include "LayerRegistry.h"
#include "Model.h"
#include "ProjectSettings.h"
#include "Renderer/UploadStaging.h"
#include "SceneSerializer.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "Window.h"
#include "World.h"
#include "gl.h"

#include <glm/gtc/matrix_transform.hpp>
#include <json.hpp>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr int kFrames = 8;       // frames per atlas side (Jacob's pick: 8 x 8 on a 2K atlas)
constexpr int kFrameSize = 256;  // px per frame in the atlas
// Rendered at kSuper x the frame size, box-filtered down: 16 samples a texel give the coverage 17 steps, and the leaf
// cards' own alpha test runs about as finely as the live trees' near the impostor's start distance (at 2x the needles
// came out sparse).
constexpr int kSuper = 4;

// ImpostorCommon.glsl's functions, mirrored exactly.
glm::vec3 HemiOctDecode(glm::vec2 p) {
    glm::vec3 d = glm::vec3(p.x + p.y, 0.0f, p.x - p.y) * 0.5f;
    d.y = 1.0f - std::abs(d.x) - std::abs(d.z);
    return glm::normalize(d);
}
void FrameBasis(const glm::vec3& d, glm::vec3& right, glm::vec3& up) {
    right = std::abs(d.y) < 0.999f ? glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), d)) : glm::vec3(1.0f, 0.0f, 0.0f);
    up = glm::cross(d, right);
}

struct Part {
    std::shared_ptr<Model> model;
    std::vector<std::shared_ptr<MaterialAsset>> materials;
    glm::mat4 local; // mesh -> the group's own space
};

struct Bake {
    std::string name;
    std::vector<std::string> models; // the LOD 0's model paths, sorted (the key)
    std::vector<Part> parts;
    int groups = 0; // how many LOD groups share it
};

// "…/Cedrus_tree_var13@Cedrus_tree_var12_LOD0.fbx" -> "Cedrus_tree_var13".
std::string BakeName(const std::string& modelPath) {
    std::string stem = std::filesystem::path(modelPath).stem().string();
    if (const size_t at = stem.find('@'); at != std::string::npos) stem.resize(at);
    return stem;
}

float SrgbEncode(float l) {
    l = std::clamp(l, 0.0f, 1.0f);
    return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}
unsigned char Byte(float v) { return (unsigned char)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); }

// One target's frame after the box filter: 4 floats a texel, coverage separate.
struct Tile {
    std::vector<glm::vec4> v; // kFrameSize^2
};

// Box-filters a kSuper x kSuper-sized frame of `src` (RGBA float, rows bottom-up as glReadPixels gives them) into a
// tile: each texel the mean of its covered samples (alpha target 0's > 0), `cov` the covered share.
void Downsample(const std::vector<float>& src, const std::vector<float>& coverSrc, Tile& out, std::vector<float>* cov) {
    const int big = kFrameSize * kSuper;
    out.v.assign((size_t)kFrameSize * kFrameSize, glm::vec4(0.0f));
    if (cov) cov->assign((size_t)kFrameSize * kFrameSize, 0.0f);
    for (int y = 0; y < kFrameSize; ++y)
        for (int x = 0; x < kFrameSize; ++x) {
            glm::vec4 sum(0.0f);
            int n = 0;
            for (int sy = 0; sy < kSuper; ++sy)
                for (int sx = 0; sx < kSuper; ++sx) {
                    const size_t i = (size_t)(y * kSuper + sy) * big + (x * kSuper + sx);
                    if (coverSrc[i * 4 + 3] <= 0.0f) continue;
                    sum += glm::vec4(src[i * 4], src[i * 4 + 1], src[i * 4 + 2], src[i * 4 + 3]);
                    ++n;
                }
            const size_t o = (size_t)y * kFrameSize + x;
            if (n) out.v[o] = sum / (float)n;
            if (cov) (*cov)[o] = (float)n / (float)(kSuper * kSuper);
        }
}

// Fills every uncovered texel of the tiles from its covered (or already filled) 8-neighbours, ring by ring, so
// bilinear filtering and mips at a silhouette blend toward the surface's own colour, never toward black.
void Dilate(std::vector<Tile*> tiles, const std::vector<float>& cov) {
    const int n = kFrameSize;
    std::vector<unsigned char> filled((size_t)n * n);
    for (size_t i = 0; i < filled.size(); ++i) filled[i] = cov[i] > 0.0f;
    std::vector<unsigned char> next = filled;
    std::vector<glm::vec4> scratch;
    for (int pass = 0; pass < n; ++pass) {
        bool any = false;
        for (Tile* t : tiles) {
            scratch = t->v;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const size_t o = (size_t)y * n + x;
                    if (filled[o]) continue;
                    glm::vec4 sum(0.0f);
                    int k = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = x + dx, yy = y + dy;
                            if ((dx | dy) == 0 || xx < 0 || yy < 0 || xx >= n || yy >= n) continue;
                            const size_t q = (size_t)yy * n + xx;
                            if (!filled[q]) continue;
                            sum += t->v[q];
                            ++k;
                        }
                    if (k) { scratch[o] = sum / (float)k; next[o] = 1; any = true; }
                }
            t->v.swap(scratch);
        }
        filled = next;
        if (!any) break;
    }
}

bool WritePng(const std::filesystem::path& file, int size, const std::vector<unsigned char>& rgba) {
    stbi_write_png_compression_level = 9;
    return stbi_write_png(file.string().c_str(), size, size, 4, rgba.data(), size * 4) != 0;
}

} // namespace

int BakeImpostors(const std::vector<std::string>& scenes, const std::string& outArg, const std::string& matchArg,
                  const char* argv0) {
    namespace fs = std::filesystem;
    using json = nlohmann::json;
    const std::string match = matchArg.empty() ? "Cedrus_Tree_mesh" : matchArg;
    if (scenes.empty() || outArg.empty()) {
        std::cerr << "[ImpostorBake] usage: --bake-impostors <scene.json>... --out <dir> [--match <text>]\n";
        return 2;
    }
    const fs::path outDir = fs::absolute(outArg);
    EnginePaths::Init(argv0);
    LayerRegistry::Load();
    AssetDatabase::ScanProject();
    ProjectSettings::Load();
    ShaderLibrary::Init(EnginePaths::Resolve("assets/shaders"));
    Window window(64, 64, "bake-impostors"); // hidden: the GL context models and textures upload into
    AssetLibrary assets;
    const auto drain = [&] { for (int k = 0; k < 20000 && assets.AsyncInFlight() > 0; ++k) { assets.PumpAsync(50.0); UploadStaging::Finish(); } };

    // Gather the distinct LOD 0s. The worlds stay alive (the parts hold their models and materials).
    std::vector<std::unique_ptr<World>> worlds;
    std::map<std::vector<std::string>, Bake> bakes;
    for (const std::string& s : scenes) {
        auto world = std::make_unique<World>();
        const fs::path abs = fs::absolute(s).lexically_normal();
        if (!SceneSerializer::Load(*world, assets, abs.string(), /*persistMigration=*/false)) {
            std::cerr << "[ImpostorBake] FAILED to load " << abs.generic_string() << "\n";
            return 1;
        }
        drain();
        World& w = *world;
        auto groups = w.Registry.view<LODGroupComponent, HierarchyComponent>();
        for (entt::entity g : groups) {
            const auto& kids = groups.get<HierarchyComponent>(g).Children;
            if (kids.empty()) continue;
            std::vector<Part> parts;
            std::vector<std::string> paths;
            bool matched = false;
            // The group's own space, its scale included: the copies of a tree are scaled (the Forest's by 0.8-1.25, on
            // their prefab roots), and the draw applies each copy's own scale to the shared bake (ImpostorRenderer).
            const glm::mat4 toGroup = glm::inverse(w.ComposeWorldTransform(g));
            std::vector<entt::entity> stack{kids[0]};
            while (!stack.empty()) {
                const entt::entity e = stack.back();
                stack.pop_back();
                if (!w.Registry.valid(e) || w.Registry.all_of<InactiveTag>(e)) continue;
                if (const auto* r = w.Registry.try_get<RenderableComponent>(e); r && r->ModelRef &&
                    r->CastShadows != RenderableComponent::ShadowCasting::ShadowsOnly) {
                    const std::string p = ProjectPaths::Relativize(r->ModelRef->Path());
                    matched |= p.find(match) != std::string::npos;
                    paths.push_back(p);
                    parts.push_back({r->ModelRef, r->Materials, toGroup * w.ComposeWorldTransform(e)});
                }
                if (const auto* h = w.Registry.try_get<HierarchyComponent>(e))
                    stack.insert(stack.end(), h->Children.begin(), h->Children.end());
            }
            if (!matched) continue;
            std::sort(paths.begin(), paths.end());
            Bake& b = bakes[paths];
            if (b.groups++ == 0) {
                b.models = paths;
                b.parts = std::move(parts);
                for (const std::string& p : paths)
                    if (p.find(match) != std::string::npos) { b.name = BakeName(p); break; }
            }
        }
        worlds.push_back(std::move(world));
    }
    if (bakes.empty()) {
        std::cerr << "[ImpostorBake] no LOD group's LOD 0 draws a model matching '" << match << "'\n";
        return 1;
    }
    // Two LOD 0s with the same first model name (a prefab variant with an extra part) get a suffix.
    {
        std::map<std::string, int> seen;
        for (auto& [key, b] : bakes)
            if (const int n = seen[b.name]++; n > 0) b.name += "_" + std::to_string(n + 1);
    }

    Shader shader(ShaderLibrary::ReadFile("ImpostorBake.vert.glsl"), ShaderLibrary::ReadFile("ImpostorBake.frag.glsl"),
                  "ImpostorBake");
    const int big = kFrameSize * kSuper;
    GLuint fbo = 0, tex[3] = {}, depth = 0;
    glCreateFramebuffers(1, &fbo);
    glCreateTextures(GL_TEXTURE_2D, 3, tex);
    for (int i = 0; i < 3; ++i) {
        glTextureStorage2D(tex[i], 1, GL_RGBA16F, big, big);
        glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + i, tex[i], 0);
    }
    glCreateTextures(GL_TEXTURE_2D, 1, &depth);
    glTextureStorage2D(depth, 1, GL_DEPTH_COMPONENT32F, big, big);
    glNamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, depth, 0);
    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glNamedFramebufferDrawBuffers(fbo, 3, bufs);
    if (glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[ImpostorBake] framebuffer incomplete\n";
        return 1;
    }

    std::error_code ec;
    fs::create_directories(outDir, ec);
    json manifest;
    manifest["frames"] = kFrames;
    manifest["frameSize"] = kFrameSize;
    manifest["layout"] = "hemi-octahedral; frame (i, j) at p = (i, j) / (frames - 1) * 2 - 1 (ImpostorCommon.glsl), "
                         "spanning its frameExtents";
    json& list = manifest["impostors"] = json::object();
    const int atlas = kFrames * kFrameSize;
    int failed = 0;
    for (auto& [key, b] : bakes) {
        // The bounding sphere of the LOD 0's boxes, in the group's space.
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const Part& p : b.parts) {
            const glm::vec3 bmin = p.model->BoundsMin(), bmax = p.model->BoundsMax();
            if (!(bmin.x <= bmax.x)) continue;
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 corner((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z);
                const glm::vec3 q = glm::vec3(p.local * glm::vec4(corner, 1.0f));
                lo = glm::min(lo, q);
                hi = glm::max(hi, q);
            }
        }
        const glm::vec3 centre = (lo + hi) * 0.5f;
        const float radius = glm::length(hi - lo) * 0.5f;
        if (!(radius > 1e-4f)) { ++failed; continue; }

        std::vector<unsigned char> outAlbedo((size_t)atlas * atlas * 4), outNormal(outAlbedo.size()), outMask(outAlbedo.size());
        std::vector<float> px[3];
        for (auto& v : px) v.resize((size_t)big * big * 4);
        std::vector<glm::vec2> extents((size_t)kFrames * kFrames, glm::vec2(radius));
        for (int fj = 0; fj < kFrames; ++fj)
            for (int fi = 0; fi < kFrames; ++fi) {
                const glm::vec2 p = glm::vec2(fi, fj) / (float)(kFrames - 1) * 2.0f - 1.0f;
                const glm::vec3 d = HemiOctDecode(p);
                glm::vec3 right, up;
                FrameBasis(d, right, up);
                const glm::mat4 view = glm::lookAt(centre + d * (2.0f * radius), centre, up);
                const auto render = [&](float ex, float ey, int targets) {
                    const glm::mat4 proj = glm::ortho(-ex, ex, -ey, ey, 0.5f * radius, 3.5f * radius);
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    glViewport(0, 0, big, big);
                    const float zero[4] = {0, 0, 0, 0};
                    for (int i = 0; i < 3; ++i) glClearNamedFramebufferfv(fbo, GL_COLOR, i, zero);
                    glDepthMask(GL_TRUE);
                    glClear(GL_DEPTH_BUFFER_BIT);
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(GL_LESS);
                    glDepthMask(GL_TRUE);
                    glDisable(GL_CULL_FACE);
                    glDisable(GL_BLEND);
                    GLStateCache::Invalidate();
                    shader.Bind();
                    shader.SetMat4("uViewProj", proj * view);
                    shader.SetVec3("uViewDir", d);
                    shader.SetVec3("uCentre", centre);
                    shader.SetFloat("uRadius", radius);
                    for (const Part& part : b.parts) {
                        shader.SetMat4("uModel", part.local);
                        shader.SetMat4("uNormalMatrix", glm::mat4(glm::transpose(glm::inverse(glm::mat3(part.local)))));
                        part.model->Draw(shader, part.materials);
                    }
                    for (int i = 0; i < targets; ++i) {
                        glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
                        glReadPixels(0, 0, big, big, GL_RGBA, GL_FLOAT, px[i].data());
                    }
                };
                // The frame fitted to the tree as this view sees it, each axis on its own: a first look over the whole
                // sphere finds where the coverage is, the frame then spans only that (centred, a texel to spare). A
                // Cedrus seen from the side is ~22 m by ~10 m of its 26 m sphere, from above ~10 m square - 1.2-2.5x
                // the texels on the tree for the same atlas.
                render(radius, radius, 1);
                glm::vec2 ext(0.0f);
                for (int y = 0; y < big; ++y)
                    for (int x = 0; x < big; ++x)
                        if (px[0][((size_t)y * big + x) * 4 + 3] > 0.0f)
                            ext = glm::max(ext, glm::abs(glm::vec2((x + 0.5f) / big, (y + 0.5f) / big) * 2.0f - 1.0f) * radius);
                if (ext.x <= 0.0f || ext.y <= 0.0f) ext = glm::vec2(radius); // nothing in view: the sphere
                ext = glm::clamp(ext + 2.0f * radius / (float)kFrameSize, glm::vec2(0.02f * radius), glm::vec2(radius));
                extents[(size_t)fj * kFrames + fi] = ext;
                render(ext.x, ext.y, 3);
                Tile t[3];
                std::vector<float> cov;
                Downsample(px[0], px[0], t[0], &cov);
                Downsample(px[1], px[0], t[1], nullptr);
                Downsample(px[2], px[0], t[2], nullptr);
                Dilate({&t[0], &t[1], &t[2]}, cov);
                // Into the atlas, top row of the image first (PNG), frame (fi, fj) at column fi, row fj from the top.
                for (int y = 0; y < kFrameSize; ++y)
                    for (int x = 0; x < kFrameSize; ++x) {
                        const size_t s = (size_t)y * kFrameSize + x;
                        const int ay = fj * kFrameSize + (kFrameSize - 1 - y), ax = fi * kFrameSize + x;
                        const size_t o = ((size_t)ay * atlas + ax) * 4;
                        const glm::vec4 a = t[0].v[s], n = t[1].v[s], m = t[2].v[s];
                        outAlbedo[o] = Byte(SrgbEncode(a.r));
                        outAlbedo[o + 1] = Byte(SrgbEncode(a.g));
                        outAlbedo[o + 2] = Byte(SrgbEncode(a.b));
                        outAlbedo[o + 3] = Byte(cov[s]);
                        const glm::vec3 nn = glm::normalize(glm::vec3(n) * 2.0f - 1.0f + glm::vec3(0, 1e-6f, 0)) * 0.5f + 0.5f;
                        outNormal[o] = Byte(nn.x);
                        outNormal[o + 1] = Byte(nn.y);
                        outNormal[o + 2] = Byte(nn.z);
                        outNormal[o + 3] = Byte(n.a);
                        for (int c = 0; c < 4; ++c) outMask[o + c] = Byte(m[c]);
                    }
            }
        const std::string files[3] = {"T_" + b.name + "_Impostor_Albedo.png", "T_" + b.name + "_Impostor_NormalDepth.png",
                                      "T_" + b.name + "_Impostor_Mask.png"};
        const bool ok = WritePng(outDir / files[0], atlas, outAlbedo) && WritePng(outDir / files[1], atlas, outNormal) &&
                        WritePng(outDir / files[2], atlas, outMask);
        if (!ok) ++failed;
        json j;
        j["lod0Models"] = b.models;
        j["groups"] = b.groups;
        j["centre"] = {centre.x, centre.y, centre.z};
        j["radius"] = radius;
        // Each frame's half-width and half-height on its image plane (frame (i, j) at j x frames + i): the frame
        // spans [-x, x] x [-y, y] around the sphere's centre.
        j["frameExtents"] = json::array();
        for (const glm::vec2& e : extents) j["frameExtents"].push_back({e.x, e.y});
        j["boundsMin"] = {lo.x, lo.y, lo.z}; // the LOD 0's box: the draw fits each card to its outline
        j["boundsMax"] = {hi.x, hi.y, hi.z};
        // The card's sway (ImpostorRenderer): the slow bend of the LOD 0's foliage materials - their flutter is too fine
        // to see on a card - as FoliageWind.glsl scales it: WindStrength x (1 - WindFlutter) at WindHeight.
        float windBend = 0.0f, windHeight = 0.0f;
        for (const Part& part : b.parts)
            for (int m = 0; m < part.model->MeshCount(); ++m) {
                const Material& mat = m < (int)part.materials.size() && part.materials[m] ? part.materials[m]->Mat
                                                                                         : part.model->MeshMaterial(m);
                if (!mat.Foliage) continue;
                const float bend = std::abs(mat.WindStrength) * (1.0f - std::clamp(mat.WindFlutter, 0.0f, 1.0f));
                if (bend > windBend) {
                    windBend = bend;
                    windHeight = mat.WindHeight;
                }
            }
        j["windBend"] = windBend;
        j["windHeight"] = windHeight;
        j["albedo"] = files[0];
        j["normalDepth"] = files[1];
        j["mask"] = files[2];
        list[b.name] = j;
        std::printf("[ImpostorBake] %s: %d groups, %zu parts, radius %.2f m, centre %.2f %.2f %.2f%s\n", b.name.c_str(),
                    b.groups, b.parts.size(), radius, centre.x, centre.y, centre.z, ok ? "" : "  (WRITE FAILED)");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(3, tex);
    glDeleteTextures(1, &depth);
    {
        std::ofstream f(outDir / "impostors.json");
        f << manifest.dump(1);
    }
    std::printf("[ImpostorBake] %zu impostors -> %s, %d failed\n", bakes.size(), outDir.generic_string().c_str(), failed);
    std::fflush(stdout);
    return failed ? 1 : 0;
}
