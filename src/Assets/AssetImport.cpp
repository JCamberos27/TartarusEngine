#include "AssetImport.h"
#include "AtomicFile.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace AssetImport {

namespace {
std::string LowerExt(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}
} // namespace

std::string ImportKind(const std::string& path) {
    const std::string ext = LowerExt(path);
    if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") return "model";
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") return "texture";
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") return "sound";
    if (ext == ".prefab") return "prefab";
    return {};
}

bool IsSourceOnlyFile(const std::string& path) {
    static const char* kSkip[] = {".blend", ".blend1", ".blend2", ".max", ".ma", ".mb", ".c4d",
                                  ".ztl", ".zpr", ".spp", ".sbs", ".psd", ".xcf", ".kra",
                                  ".zip", ".rar", ".7z", ".unitypackage", ".uasset"};
    const std::string ext = LowerExt(path);
    if (std::any_of(std::begin(kSkip), std::end(kSkip), [&](const char* s) { return ext == s; })) return true;
    std::string name = fs::path(path).filename().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return name == "thumbs.db" || name == "desktop.ini" || name == ".ds_store";
}

TextureKind GuessTextureKind(const std::string& path) {
    // Lower-case words of the stem, split on anything that isn't a letter or digit.
    std::vector<std::string> words;
    std::string cur;
    for (char c : fs::path(path).stem().string()) {
        if (std::isalnum((unsigned char)c)) cur += (char)std::tolower((unsigned char)c);
        else if (!cur.empty()) { words.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) words.push_back(cur);

    static const char* kQualifiers[] = {"opengl", "gl", "ogl", "directx", "dx", "unity", "unreal", "ue", "ue4",
                                        "ue5", "hd", "lod0", "hi", "high", "low", "final", "baked"};
    auto isQualifier = [&](const std::string& w) {
        if (std::all_of(w.begin(), w.end(), [](char c) { return std::isdigit((unsigned char)c); })) return true; // UDIM, 1024
        if (w.size() <= 3 && w.back() == 'k' && std::isdigit((unsigned char)w[0])) return true;                 // 2k, 4k
        return std::any_of(std::begin(kQualifiers), std::end(kQualifiers), [&](const char* q) { return w == q; });
    };
    static const char* kNormal[] = {"normal", "normals", "normalmap", "nrm", "nor", "norm", "nm", "nml", "normalgl", "normaldx"};
    static const char* kData[] = {"roughness", "rough", "rgh", "metallic", "metalness", "metal", "ao",
                                  "occlusion", "ambientocclusion", "height", "heightmap", "displacement", "disp",
                                  "bump", "mask", "maskmap", "orm", "rma", "arm", "mra", "smoothness", "gloss",
                                  "glossiness", "opacity", "cavity", "curvature", "thickness", "rm", "mm", "hm"};
    auto in = [](const std::string& w, const auto& list) {
        return std::any_of(std::begin(list), std::end(list), [&](const char* x) { return w == x; });
    };
    // The last two meaningful words ("Ambient_Occlusion", "Normal_Map", "Base_Color").
    for (size_t i = words.size(); i-- > 0;) {
        if (isQualifier(words[i])) continue;
        const std::string& w = words[i];
        const std::string pair = i > 0 ? words[i - 1] + w : w;
        if (in(w, kNormal) || in(pair, kNormal)) return TextureKind::Normal;
        if (in(w, kData) || in(pair, kData)) return TextureKind::Data;
        return TextureKind::Color;
    }
    return TextureKind::Color;
}

FolderCopy CopyFolderInto(const std::string& sourceDir, const std::string& destParent) {
    FolderCopy out;
    std::error_code ec;
    const fs::path src = fs::absolute(sourceDir, ec).lexically_normal();
    if (ec || !fs::is_directory(src, ec)) { out.Error = "not a folder"; return out; }
    fs::path name = src.filename();
    if (name.empty()) name = src.parent_path().filename(); // "C:/Pack/" -> "Pack"

    fs::create_directories(destParent, ec);
    fs::path dest = fs::path(destParent) / name;
    for (int n = 2; fs::exists(dest, ec); ++n) dest = fs::path(destParent) / (name.string() + " (" + std::to_string(n) + ")");
    fs::create_directories(dest, ec);
    if (ec) { out.Error = ec.message(); return out; }
    out.Folder = dest.string();

    for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        const fs::path rel = it->path().lexically_relative(src);
        if (it->is_directory(fec)) { fs::create_directories(dest / rel, fec); continue; }
        if (!it->is_regular_file(fec)) continue;
        if (IsSourceOnlyFile(it->path().string())) { ++out.Skipped; continue; }
        const fs::path to = dest / rel;
        fs::create_directories(to.parent_path(), fec);
        // Files the import queue loads are the editor's own write, so the watcher doesn't import
        // them a second time. Companions (.mat, ...) are left to the watcher, which lists them.
        if (!ImportKind(to.string()).empty()) AtomicFile::NoteSelfWrite(to);
        if (!fec) fs::copy_file(it->path(), to, fs::copy_options::skip_existing, fec);
        if (fec) {
            if (out.Error.empty()) out.Error = "'" + it->path().string() + "': " + fec.message();
            continue;
        }
        out.Files.push_back(to.string());
    }
    if (ec && out.Error.empty()) out.Error = ec.message();
    return out;
}

} // namespace AssetImport
