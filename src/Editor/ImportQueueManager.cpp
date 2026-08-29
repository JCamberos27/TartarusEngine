#include "ImportQueueManager.h"
#include "Log.h"
#include <imgui.h>
#include <filesystem>
#include <algorithm>
#include <map>

namespace {

std::string ClassifyExtension(const std::string& ext) {
    if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") return "model";
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") return "texture";
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") return "sound";
    if (ext == ".prefab") return "prefab";
    if (ext == ".json") return "scene";
    return "other";
}

} // namespace

void ImportQueueManager::Enqueue(const std::vector<std::string>& paths) {
    if (paths.empty()) return;

    std::map<std::string, int> counts; // sorted by kind name for a stable, readable summary order
    for (const auto& p : paths) {
        std::string ext = std::filesystem::path(p).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        counts[ClassifyExtension(ext)]++;
    }
    std::string summary;
    for (const auto& [kind, count] : counts) {
        if (!summary.empty()) summary += ", ";
        summary += std::to_string(count) + " " + kind + (count == 1 ? "" : "s");
    }
    Log::Info("Queued " + summary + " for import...");

    m_TotalInBatch += (int)paths.size();
    m_Pending.insert(m_Pending.end(), paths.begin(), paths.end());
}

void ImportQueueManager::Update(const ImportFn& importOne, int perFrame) {
    for (int i = 0; i < perFrame && !m_Pending.empty(); ++i) {
        std::string path = m_Pending.front();
        m_Pending.pop_front();
        m_LastProcessed = std::filesystem::path(path).filename().string();
        if (importOne) importOne(path);
        m_ProcessedInBatch++;
    }
    if (m_Pending.empty()) {
        m_TotalInBatch = 0;
        m_ProcessedInBatch = 0;
    }
}

void ImportQueueManager::CancelRemaining() {
    if (m_Pending.empty()) return;
    Log::Warn("Cancelled " + std::to_string(m_Pending.size()) + " remaining queued import(s).");
    m_Pending.clear();
    m_TotalInBatch = 0;
    m_ProcessedInBatch = 0;
}

void ImportQueueManager::DrawProgressUI() {
    if (!IsActive()) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 size(360.0f, 0.0f);
    ImVec2 pos(viewport->WorkPos.x + viewport->WorkSize.x - size.x - 12.0f,
        viewport->WorkPos.y + viewport->WorkSize.y - 90.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##ImportQueueProgress", nullptr, flags)) {
        int processed = m_TotalInBatch - (int)m_Pending.size();
        ImGui::Text("Importing Assets...");
        ImGui::TextDisabled("Processing %s (%d/%d)", m_LastProcessed.c_str(), processed, m_TotalInBatch);

        float fraction = m_TotalInBatch > 0 ? (float)processed / (float)m_TotalInBatch : 0.0f;
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));

        if (ImGui::Button("Cancel Remaining", ImVec2(-1.0f, 0.0f))) {
            CancelRemaining();
        }
    }
    ImGui::End();
}
