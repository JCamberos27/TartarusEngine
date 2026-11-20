// Model textures: the loaded-texture cache, embedded textures and the search for a texture an imported file names
// (the model folder, loose names, texture sets).

#include "Model.h"
#include "Log.h"
#include "AssetDatabase.h"
#include "Texture.h"
#include "DefaultTextures.h"

#include <assimp/scene.h>

#include <algorithm>
#include <filesystem>
#include <cctype>
#include <cstring>
#include <mutex>
#include <chrono>
#include <unordered_map>

std::shared_ptr<Texture> Model::LoadCachedTexture(const std::string& fullPath, TextureRole role) {
    // #95 — the role decides the colour space. Every map used to be loaded with the default
    // (sRGB) settings, so normal / metallic / roughness / AO maps were gamma-decoded on sample:
    // bent normals and wrong roughness on essentially every imported model. Only albedo and
    // emissive are colour data. Keyed by path + role, in case one file feeds both kinds of slot.
    const std::string key = fullPath + (role == TextureRole::Color ? "|srgb" : role == TextureRole::Normal ? "|normal" : "|linear");
    auto it = m_D->TextureCache.find(key);
    if (it != m_D->TextureCache.end()) return it->second;

    // Shared between models while any of them holds it: a weapon's animation files each carry its whole mesh and
    // material, so every clip file decoded and uploaded the same 4096^2 maps again (5-10 ms of GL upload per map on
    // the main thread, and ~85 MB of VRAM per copy). Keyed on the file's write time too, so a changed file is
    // read anew. A hit is returned as is: another thread may be uploading it, so its state isn't looked at.
    static std::mutex sharedMutex;
    static std::unordered_map<std::string, std::weak_ptr<Texture>> shared;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(std::filesystem::u8path(fullPath), ec);
    const std::string sharedKey = AssetDatabase::PathKey(fullPath) + key.substr(fullPath.size()) + "|" +
                                  (ec ? std::string("?") : std::to_string(stamp.time_since_epoch().count()));
    {
        std::lock_guard<std::mutex> lock(sharedMutex);
        if (auto sit = shared.find(sharedKey); sit != shared.end()) {
            if (std::shared_ptr<Texture> existing = sit->second.lock()) {
                m_D->TextureCache[key] = existing;
                return existing;
            }
        }
    }

    TextureImportSettings settings;
    settings.IsSRGB = role == TextureRole::Color;
    if (role == TextureRole::Normal) settings.TextureType = TextureImportSettings::Type::NormalMap;
    // Deferred: decode now (this thread), upload in FinishGpuUploadStep.
    auto tex = m_DeferGpu ? Texture::CreatePending(Texture::DecodeFile(fullPath, settings))
                          : std::make_shared<Texture>(fullPath, settings);
    if (!tex || (!tex->IsValid() && !tex->IsPendingUpload())) return nullptr;
    m_D->TextureCache[key] = tex;
    if (!ec) {
        std::lock_guard<std::mutex> lock(sharedMutex);
        std::weak_ptr<Texture>& slot = shared[sharedKey];
        if (std::shared_ptr<Texture> raced = slot.lock()) return m_D->TextureCache[key] = raced; // another model got here first
        slot = tex;
        if (shared.size() > 4096)
            for (auto s = shared.begin(); s != shared.end();) s = s->second.expired() ? shared.erase(s) : std::next(s);
    }
    return tex;
}

std::string Model::ResolveTexturePath(const std::string& raw) const {
    return ResolveTexturePathIn(m_D->Directory, raw);
}

namespace {
// "Foo_Normal.PNG" -> "foo_normal.png".
std::string LowerTextureName(const std::filesystem::path& name) {
    std::string s = name.string();
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// The stem with case, separators and punctuation dropped: "AKS74U_Albedo-Transparency.png" ->
// "aks74ualbedotransparency". Two names with the same key are the same texture under a
// different spelling or format.
std::string LooseTextureKey(const std::filesystem::path& name) {
    std::string out;
    for (char c : name.stem().string())
        if (std::isalnum((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
    return out;
}

bool IsTextureFileExt(std::string ext) {
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" ||
           ext == ".psd" || ext == ".gif" || ext == ".hdr" || ext == ".pic" || ext == ".pnm";
}

std::string LooseName(const std::string& name); // below

// Every image file up to three folders below `root`, shallowest first. Scene loads resolve many
// textures for many models in the same few folders, so listings are cached briefly: long enough
// to cover one load or one import batch, short enough that files copied in a moment ago show up.
// The walk is capped so a model sitting near a huge folder can't stall an import. Each file carries the
// lowered / loose names the lookups compare (worked out once per listing, not per lookup per file).
struct ListedImage {
    std::filesystem::path Path;
    std::string Lower;    // LowerTextureName(filename)
    std::string Loose;    // LooseTextureKey(filename)
    std::string SetLoose; // the stem without a "T_" prefix, loose (FindTextureSetMap)
};
std::shared_ptr<const std::vector<ListedImage>> ImageFilesUnder(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    struct Listing { Clock::time_point Built; std::shared_ptr<const std::vector<ListedImage>> Files; };
    static std::mutex s_Mutex;
    static std::map<std::string, Listing> s_Cache;
    constexpr auto kLifetime = std::chrono::seconds(3);
    constexpr int kMaxDepth = 3;
    constexpr size_t kMaxVisited = 20000;

    const std::string key = root.lexically_normal().generic_string();
    const Clock::time_point now = Clock::now();
    std::lock_guard<std::mutex> lock(s_Mutex);
    auto it = s_Cache.find(key);
    if (it != s_Cache.end() && now - it->second.Built < kLifetime) return it->second.Files;

    std::vector<std::pair<int, fs::path>> found;
    std::error_code ec;
    if (fs::is_directory(root, ec)) {
        size_t visited = 0;
        fs::recursive_directory_iterator walk(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && walk != end && visited < kMaxVisited; walk.increment(ec), ++visited) {
            std::error_code fec;
            if (walk->is_directory(fec)) {
                if (walk.depth() >= kMaxDepth - 1) walk.disable_recursion_pending();
                continue;
            }
            if (walk->is_regular_file(fec) && IsTextureFileExt(walk->path().extension().string()))
                found.emplace_back(walk.depth(), walk->path());
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto files = std::make_shared<std::vector<ListedImage>>();
    files->reserve(found.size());
    for (auto& f : found) {
        ListedImage li;
        li.Lower = LowerTextureName(f.second.filename());
        li.Loose = LooseTextureKey(f.second.filename());
        std::string stem = f.second.stem().string();
        if (stem.size() > 2 && (stem[0] == 'T' || stem[0] == 't') && stem[1] == '_') stem = stem.substr(2);
        li.SetLoose = LooseName(stem);
        li.Path = std::move(f.second);
        files->push_back(std::move(li));
    }
    if (s_Cache.size() > 256) s_Cache.clear();
    s_Cache[key] = Listing{now, files};
    return files;
}

// Where a model's textures are looked for: its own folder tree, then the folder above - but only
// when the model sits in a models-only folder of its asset (<Asset>/Models/x.fbx beside
// <Asset>/Textures/) or its own tree has no images at all. A model placed directly in its asset
// folder (<Category>/<Asset>/x.fbx) must not reach into the whole category and pick up a
// neighbouring asset's texture.
std::vector<std::filesystem::path> TextureSearchRoots(const std::filesystem::path& modelDir) {
    namespace fs = std::filesystem;
    std::vector<fs::path> roots{modelDir};
    const fs::path parent = modelDir.parent_path();
    if (parent.empty() || parent == modelDir || parent == parent.root_path()) return roots;
    std::string name = modelDir.filename().string();
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    static const char* kModelFolders[] = {"models", "model", "meshes", "mesh", "fbx", "obj",
                                          "geometry", "geo", "export", "exports"};
    const bool modelsFolder = std::any_of(std::begin(kModelFolders), std::end(kModelFolders),
                                          [&](const char* f) { return name == f; });
    if (modelsFolder || ImageFilesUnder(modelDir)->empty()) roots.push_back(parent);
    return roots;
}

// LooseTextureKey for a name that isn't a file name (a material's).
std::string LooseName(const std::string& name) {
    std::string out;
    for (char c : name)
        if (std::isalnum((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
    return out;
}
} // namespace

namespace {
// "M_Garage_Props_Mat" -> "garageprops": the material's texture-set name, compared loosely.
std::string TextureSetKey(std::string name) {
    auto lower = [](std::string v) { for (char& c : v) c = (char)std::tolower((unsigned char)c); return v; };
    const std::string l = lower(name);
    for (const char* pre : {"mi_", "m_", "mat_"})
        if (l.rfind(pre, 0) == 0 && l.size() > std::strlen(pre)) { name = name.substr(std::strlen(pre)); break; }
    const std::string l2 = lower(name);
    for (const char* suf : {"_material", "_mat", "_mtl"}) {
        const size_t n = std::strlen(suf);
        if (l2.size() > n && l2.compare(l2.size() - n, n, suf) == 0) { name = name.substr(0, name.size() - n); break; }
    }
    return LooseName(name);
}
} // namespace

std::string Model::FindTextureSetMap(const std::string& modelDirStr, const std::vector<std::string>& setNames,
                                     TextureSetMap kind) {
    namespace fs = std::filesystem;
    // Map-type suffixes, loose, longest first so "albedotransparency" wins over "albedo".
    static const std::vector<std::vector<std::string>> kSuffixes = {
        {"albedotransparency", "basecolour", "basecolor", "albedo", "diffuse", "colour", "color", "diff"},
        {"normalopengl", "normalgl", "normal", "norgl", "norm", "nrm", "nor"},
        {"metallic", "metalness", "metal"},
        {"roughness", "rough"},
        {"ambientocclusion", "occlusion", "ao"},
        {"emissive", "emission"},
    };
    const std::vector<std::string>& suffixes = kSuffixes[(size_t)kind];

    // Each name as written ("Mat_Sofa_Vintage_03" is that set's real name), then without its
    // material prefix / suffix ("Door_Mat" -> "door").
    std::vector<std::string> keys;
    auto addKey = [&](const std::string& k) {
        if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    };
    for (const std::string& n : setNames) {
        addKey(LooseName(n));
        addKey(TextureSetKey(n));
    }
    if (keys.empty()) return {};

    const std::vector<fs::path> roots = TextureSearchRoots(fs::path(modelDirStr));

    // Every map of this kind nearby, as {set name, file}, shallowest first.
    std::vector<std::pair<std::string, fs::path>> maps;
    for (const fs::path& root : roots) {
        const auto files = ImageFilesUnder(root);
        for (const ListedImage& li : *files) {
            const std::string& loose = li.SetLoose;
            for (const std::string& suf : suffixes) {
                if (loose.size() <= suf.size() || loose.compare(loose.size() - suf.size(), suf.size(), suf) != 0) continue;
                maps.emplace_back(loose.substr(0, loose.size() - suf.size()), li.Path);
                break; // this file's map type is `suf`
            }
        }
    }

    // 1. A set named exactly like the material (or model).
    for (const std::string& key : keys)
        for (const auto& [set, f] : maps)
            if (set == key) return f.string();
    // 2. A set whose name extends it with a variant ("Bed_blanket" -> "Bed_Blanket_Clean"), but
    //    only when exactly one set does - two candidates is a guess, and a wrong texture is
    //    worse than none.
    for (const std::string& key : keys) {
        const fs::path* only = nullptr;
        std::string onlySet;
        bool ambiguous = false;
        for (const auto& [set, f] : maps) {
            if (set.size() <= key.size() || set.compare(0, key.size(), key) != 0) continue;
            if (!only) { only = &f; onlySet = set; }
            else if (set != onlySet) { ambiguous = true; break; }
        }
        if (only && !ambiguous) return only->string();
    }
    return {};
}

std::string Model::ResolveTexturePathIn(const std::string& modelDirStr, const std::string& raw) {
    namespace fs = std::filesystem;

    // Embedded texture ("*0", "*1", ...): the pixels live inside the model file, not on disk.
    // The Texture class is disk-only, so there's nothing to resolve here — hand the marker back
    // and let the caller log one clear line instead of a mangled path.
    if (!raw.empty() && raw[0] == '*') return raw;
    if (raw.empty()) return raw;

    std::error_code ec;

    // Unify separators so a Windows-authored "\" path (or a mixed "/"+"\" one) parses the same
    // way regardless of the platform doing the import.
    std::string norm = raw;
    std::replace(norm.begin(), norm.end(), '\\', '/');

    const fs::path p(norm);
    const fs::path modelDir(modelDirStr);

    // 1. Exactly as given, when it's an absolute path that actually exists on THIS machine.
    if (p.is_absolute()) {
        if (fs::exists(p, ec)) return p.lexically_normal().string();
    } else {
        // 2. Relative to the model's own directory (the common, correct case), ".." collapsed.
        const fs::path joined = (modelDir / p).lexically_normal();
        if (fs::exists(joined, ec)) return joined.string();
    }

    const fs::path filename = p.filename();
    if (!filename.empty()) {
        // 3. Just the filename, sitting next to the model. Covers texture folders that got
        //    flattened, and absolute paths from another machine that shipped a same-named file
        //    with the model (e.g. the Chesterfield Sofa pack, whose materials point at the
        //    original author's Dropbox).
        const fs::path byName = modelDir / filename;
        if (fs::exists(byName, ec)) return byName.string();

        // 4. Search the asset's own folder tree: everything under the model's folder, then under
        //    the folder above it (see TextureSearchRoots). Asset packs almost always ship as
        //    <Asset>/Models/x.fbx next to <Asset>/Textures/.../x_Normal.png, and the path the
        //    FBX baked in points at the author's machine, so neither step 2 nor 3 finds it.
        //    Within each tree the exact filename wins (case-insensitive), then a loose match
        //    that ignores case, separators and the extension ("aks74u_AlbedoTransparency.tga"
        //    finds "AKS74U_Albedo_Transparency.png"). Shallower files win ties.
        const std::vector<fs::path> roots = TextureSearchRoots(modelDir);
        const std::string wantName = LowerTextureName(filename);
        const std::string wantLoose = LooseTextureKey(filename);
        for (const fs::path& root : roots) {
            const auto files = ImageFilesUnder(root);
            for (const ListedImage& li : *files)
                if (li.Lower == wantName) return li.Path.string();
            if (wantLoose.empty()) continue;
            for (const ListedImage& li : *files)
                if (li.Loose == wantLoose) return li.Path.string();
        }
    }

    // Nothing matched — return a clean best-effort path so the load failure names something
    // sensible rather than "modelDir + someone-else's-absolute-path".
    if (p.is_absolute()) return p.lexically_normal().string();
    return (modelDir / p).lexically_normal().string();
}

std::shared_ptr<Texture> Model::LoadEmbeddedTexture(const aiTexture* tex, const std::string& ref, TextureRole role) {
    const std::string key = "*embedded:" + ref + (role == TextureRole::Color ? "|srgb" : role == TextureRole::Normal ? "|normal" : "|linear");
    auto it = m_D->TextureCache.find(key);
    if (it != m_D->TextureCache.end()) return it->second;

    std::vector<unsigned char> bytes;
    int rawW = 0, rawH = 0;
    if (tex->mHeight == 0) {
        // Compressed (PNG/JPG/...): mWidth is the byte count.
        const auto* b = reinterpret_cast<const unsigned char*>(tex->pcData);
        bytes.assign(b, b + tex->mWidth);
    } else {
        // Raw aiTexel (BGRA) -> RGBA8.
        rawW = (int)tex->mWidth; rawH = (int)tex->mHeight;
        bytes.resize((size_t)rawW * rawH * 4);
        for (size_t i = 0; i < (size_t)rawW * rawH; ++i) {
            const aiTexel& t = tex->pcData[i];
            bytes[i * 4 + 0] = t.r; bytes[i * 4 + 1] = t.g; bytes[i * 4 + 2] = t.b; bytes[i * 4 + 3] = t.a;
        }
    }
    TextureImportSettings settings;
    settings.IsSRGB = role == TextureRole::Color;
    if (role == TextureRole::Normal) settings.TextureType = TextureImportSettings::Type::NormalMap;
    auto out = m_DeferGpu
        ? Texture::CreatePending(Texture::DecodeMemory(m_Path + "#" + ref, std::move(bytes), rawW, rawH, settings))
        : std::make_shared<Texture>(m_Path + "#" + ref, std::move(bytes), rawW, rawH, settings);
    if (!out || (!out->IsValid() && !out->IsPendingUpload())) return nullptr;
    m_D->TextureCache[key] = out;
    return out;
}
