#include "MemReport.h"
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <unordered_map>
#include <unordered_set>
#include "gl.h"
#include "GpuMemTrack.h"
#include "Model.h"
#include "ModelMesh.h"
#include "Texture.h"

namespace {

std::string FileHash(const std::string& path, uint64_t& bytes) {
    static std::unordered_map<std::string, std::pair<std::string, uint64_t>> cache;
    if (auto it = cache.find(path); it != cache.end()) { bytes = it->second.second; return it->second.first; }
    std::ifstream f(path, std::ios::binary);
    uint64_t h = 1469598103934665603ull, n = 0;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        const std::streamsize got = f.gcount();
        for (std::streamsize i = 0; i < got; ++i) { h ^= (unsigned char)buf[(size_t)i]; h *= 1099511628211ull; }
        n += (uint64_t)got;
    }
    char out[32];
    std::snprintf(out, sizeof(out), "%016llx", (unsigned long long)h);
    bytes = n;
    if (n == 0) return "";
    cache[path] = {out, n};
    return out;
}

} // namespace

void MemReport::Write(const std::string& dir, const std::string& label, const std::vector<const Model*>& models) {
    using nlohmann::json;
    json j;
    j["label"] = label;

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    j["process"] = {{"workingSetMiB", pmc.WorkingSetSize / 1048576.0}, {"peakWorkingSetMiB", pmc.PeakWorkingSetSize / 1048576.0},
                    {"privateMiB", pmc.PrivateUsage / 1048576.0}, {"peakPagefileMiB", pmc.PeakPagefileUsage / 1048576.0}};

    GLint totKiB = 0, availKiB = 0; // GL_GPU_MEMORY_INFO_TOTAL/CURRENT_AVAILABLE_VIDMEM_NVX
    glGetIntegerv(0x9047, &totKiB);
    glGetIntegerv(0x9049, &availKiB);
    j["vram"] = {{"totalMiB", totKiB / 1024.0}, {"availableMiB", availKiB / 1024.0}, {"usedMiB", (totKiB - availKiB) / 1024.0}};

    double texMiB = 0, texEmbeddedMiB = 0;
    json texes = json::array();
    Texture::ForEachLive([&](const Texture& t) {
        json e;
        e["path"] = t.Path();
        e["srcW"] = t.Width(); e["srcH"] = t.Height();
        e["upW"] = t.UploadWidth(); e["upH"] = t.UploadHeight();
        e["channels"] = t.SourceChannels();
        e["format"] = t.GpuFormatName();
        e["gpuBytes"] = t.GpuBytes();
        e["embeddedBytes"] = t.EmbeddedSourceBytes();
        e["mips"] = t.ImportSettings().GenerateMipmaps;
        e["srgb"] = t.ImportSettings().IsSRGB;
        e["compressionMode"] = (int)t.ImportSettings().CompressionMode;
        std::error_code ec;
        if (!t.Path().empty() && std::filesystem::is_regular_file(t.Path(), ec)) {
            uint64_t n = 0;
            e["hash"] = FileHash(t.Path(), n);
            e["fileBytes"] = n;
        }
        texMiB += t.GpuBytes() / 1048576.0;
        texEmbeddedMiB += t.EmbeddedSourceBytes() / 1048576.0;
        texes.push_back(std::move(e));
    });
    j["textures"] = std::move(texes);
    j["textureGpuMiB"] = texMiB;
    j["textureEmbeddedSourceMiB"] = texEmbeddedMiB;

    std::unordered_set<const ModelMesh*> seenMesh;
    std::unordered_set<unsigned> seenVbo;
    std::unordered_set<const Model*> seenModel;
    json meshes = json::array();
    double meshGpuMiB = 0, meshCpuMiB = 0, skinCpuMiB = 0;
    for (const Model* m : models) {
        if (!m || !seenModel.insert(m).second) continue;
        for (int i = 0; i < m->MeshCount(); ++i) {
            const ModelMesh* mm = m->MeshPtr(i);
            if (!mm || !seenMesh.insert(mm).second) continue;
            const bool firstGpu = mm->VertexBuffer() != 0 && seenVbo.insert(mm->VertexBuffer()).second;
            const size_t gpu = firstGpu ? (size_t)mm->VertexCount() * sizeof(ModelVertex) + (size_t)mm->IndexCount() * 4 : 0;
            json e = {{"model", m->Path()}, {"mesh", i}, {"verts", mm->VertexCount()}, {"indices", mm->IndexCount()},
                      {"gpuBytes", gpu}, {"cpuBytes", mm->CpuBytes()}, {"skinCpuBytes", mm->SkinBytes()}};
            meshGpuMiB += gpu / 1048576.0;
            meshCpuMiB += mm->CpuBytes() / 1048576.0;
            skinCpuMiB += mm->SkinBytes() / 1048576.0;
            meshes.push_back(std::move(e));
        }
    }
    j["meshes"] = std::move(meshes);
    j["modelCount"] = seenModel.size();
    j["meshGpuMiB"] = meshGpuMiB;
    j["meshCpuMiB"] = meshCpuMiB;
    j["meshSkinCpuMiB"] = skinCpuMiB;
    {
        json tr = json::object();
        double trMiB = 0;
        for (const auto& [k, b] : GpuMemTrack::Snapshot()) { tr[k] = b / 1048576.0; trMiB += b / 1048576.0; }
        j["tracked"] = tr;
        j["trackedMiB"] = trMiB;
    }
    j["otherVramMiB"] = (totKiB - availKiB) / 1024.0 - texMiB - meshGpuMiB;

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream(std::filesystem::path(dir) / (label + ".json")) << j.dump(1);
}
