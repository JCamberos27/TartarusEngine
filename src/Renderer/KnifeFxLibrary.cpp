#include "KnifeFxLibrary.h"

#include "Log.h"
#include "ProjectPaths.h"
#include "gl.h"

#include <algorithm>
#include <filesystem>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2 0x8DBD
#endif
#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#endif

KnifeFxLibrary& KnifeFxLibrary::Get() {
    static KnifeFxLibrary* lib = new KnifeFxLibrary();
    return *lib;
}

int KnifeFxLibrary::Find(const std::string& name) const {
    for (size_t i = 0; i < m_Entries.size(); ++i)
        if (m_Entries[i].Name == name) return (int)i;
    return -1;
}

namespace {
unsigned Upload(const std::vector<std::uint8_t>& data, int layers, int size, int mips, unsigned format) {
    if (layers <= 0) return 0;
    unsigned tex = 0;
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &tex);
    glTextureStorage3D(tex, mips, format, size, size, layers);
    const size_t layerBytes = KnifeFxImport::LayerBytes(size, mips);
    for (int l = 0; l < layers; ++l) {
        size_t off = (size_t)l * layerBytes;
        for (int m = 0; m < mips; ++m) {
            const int s = std::max(4, size >> m);
            const size_t bytes = (size_t)(s / 4) * (s / 4) * 16;
            glCompressedTextureSubImage3D(tex, m, 0, 0, l, s, s, 1, format, (GLsizei)bytes, data.data() + off);
            off += bytes;
        }
    }
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const float aniso = 4.0f;
    glTextureParameterfv(tex, GL_TEXTURE_MAX_ANISOTROPY, &aniso);
    return tex;
}
} // namespace

bool KnifeFxLibrary::Load() {
    if (m_LoadTried) return m_Loaded;
    m_LoadTried = true;
    namespace fs = std::filesystem;
    const fs::path dir = fs::u8path(ProjectPaths::Resolve("assets/Effects/Knife"));
    size_t bytes = 0;
    for (int l = 0; l < (int)KnifeFxImport::Library::Count; ++l) {
        const auto lib = (KnifeFxImport::Library)l;
        const std::string path = (dir / KnifeFxImport::LibraryFile(lib)).u8string();
        if (!fs::exists(fs::u8path(path))) continue;
        KnifeFxImport::LibraryData data;
        std::string err;
        if (!KnifeFxImport::ReadLibrary(path, data, &err)) {
            Log::Error("KnifeFx: " + path + ": " + err);
            continue;
        }
        const auto& h = data.Header;
        m_Color[l] = Upload(data.Color, (int)h.ColorLayers, (int)h.Size, (int)h.Mips, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
        m_Normal[l] = Upload(data.Normal, (int)h.NormalLayers, (int)h.Size, (int)h.Mips, GL_COMPRESSED_RG_RGTC2);
        bytes += data.Color.size() + data.Normal.size();
        for (const auto& fe : data.Entries) {
            Entry e;
            e.Name = fe.Name;
            e.Lib = lib;
            e.ColorLayer = fe.ColorLayer;
            e.NormalLayer = fe.NormalLayer;
            e.Cols = fe.Cols;
            e.Rows = fe.Rows;
            e.Frames = fe.Frames;
            e.Flags = fe.Flags;
            e.Smoothness = fe.Smoothness;
            e.Aspect = fe.Aspect;
            m_Entries.push_back(std::move(e));
        }
    }
    if (m_Entries.empty()) {
        Log::Warn("KnifeFx: no Knife texture libraries in assets/Effects/Knife - run TartarusEngine --import-knife-fx <folder>");
        return false;
    }
    m_Loaded = true;
    Log::Info("KnifeFx: " + std::to_string(m_Entries.size()) + " entries loaded (" + std::to_string(bytes / (1024 * 1024)) + " MB)");
    return true;
}
