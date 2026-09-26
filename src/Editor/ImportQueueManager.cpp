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

    if (m_Pending.empty() && m_ProcessedInBatch == 0) { // a new batch: start its tally
        m_Imported.clear();
        m_Failed = 0;
        m_WarningsAtStart = Log::CountOf(LogLevel::Warning);
        m_ErrorsAtStart = Log::CountOf(LogLevel::Error);
        m_SummaryShownAt = -1.0;
    }
    m_TotalInBatch += (int)paths.size();
    m_Pending.insert(m_Pending.end(), paths.begin(), paths.end());
}

void ImportQueueManager::Update(const ImportFn& importOne, int perFrame) {
    for (int i = 0; i < perFrame && !m_Pending.empty(); ++i) {
        std::string path = m_Pending.front();
        m_Pending.pop_front();
        m_LastProcessed = std::filesystem::path(path).filename().string();
        const std::string kind = importOne ? importOne(path) : std::string();
        if (kind.empty()) ++m_Failed;
        else ++m_Imported[kind];
        m_ProcessedInBatch++;
    }
    if (m_Pending.empty() && m_ProcessedInBatch > 0) FinishBatch(0);
}

void ImportQueueManager::FinishBatch(int cancelled) {
    // Console clears reset the counters; never report a negative count.
    m_BatchWarnings = std::max(0, Log::CountOf(LogLevel::Warning) - m_WarningsAtStart);
    m_BatchErrors = std::max(0, Log::CountOf(LogLevel::Error) - m_ErrorsAtStart);
    std::string what;
    for (const auto& [kind, count] : m_Imported) {
        if (!what.empty()) what += ", ";
        what += std::to_string(count) + " " + kind + (count == 1 ? "" : "s");
    }
    m_Summary = "Imported " + (what.empty() ? std::string("nothing") : what);
    if (m_Failed) m_Summary += "; " + std::to_string(m_Failed) + " failed or skipped";
    if (cancelled) m_Summary += "; " + std::to_string(cancelled) + " cancelled";
    if (m_BatchWarnings || m_BatchErrors)
        m_Summary += " - " + std::to_string(m_BatchWarnings) + " warning(s), " + std::to_string(m_BatchErrors) + " error(s) in the Console";
    m_Summary += ".";
    if (m_Failed || m_BatchErrors) Log::Warn(m_Summary);
    else Log::Info(m_Summary);
    m_SummaryShownAt = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    m_TotalInBatch = 0;
    m_ProcessedInBatch = 0;
}

void ImportQueueManager::CancelRemaining() {
    if (m_Pending.empty()) return;
    const int cancelled = (int)m_Pending.size();
    m_Pending.clear();
    FinishBatch(cancelled);
}

void ImportQueueManager::DrawProgressUI(const std::function<void()>& openConsole) {
    constexpr double kSummarySeconds = 8.0;
    const bool showSummary = !IsActive() && m_SummaryShownAt >= 0.0 && ImGui::GetTime() - m_SummaryShownAt < kSummarySeconds;
    if (!IsActive() && !showSummary) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // #139 — sized from the font (which bakes in the UI scale) instead of fixed pixels, so the
    // window isn't cramped at 150% / 200% scaling. 360 px at the editor's 16 px base font.
    const float scale = ImGui::GetFontSize() / 16.0f;
    ImVec2 size(360.0f * scale, 0.0f);
    ImVec2 pos(viewport->WorkPos.x + viewport->WorkSize.x - size.x - 12.0f * scale,
        viewport->WorkPos.y + viewport->WorkSize.y - 12.0f * scale);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.0f, 1.0f)); // bottom-anchored: the summary wraps upward
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking;
    if (showSummary) {
        if (ImGui::Begin("##ImportQueueProgress", nullptr, flags)) {
            const bool problems = m_Failed || m_BatchErrors || m_BatchWarnings;
            ImGui::TextUnformatted(problems ? "Import finished with problems" : "Import finished");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", m_Summary.c_str());
            ImGui::PopTextWrapPos();
            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (problems && openConsole) {
                if (ImGui::Button("Show Console", ImVec2(half, 0.0f))) { openConsole(); m_SummaryShownAt = -1.0; }
                ImGui::SameLine();
            }
            if (ImGui::Button("Dismiss", ImVec2(problems && openConsole ? half : -1.0f, 0.0f))) m_SummaryShownAt = -1.0;
        }
        ImGui::End();
        return;
    }
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
