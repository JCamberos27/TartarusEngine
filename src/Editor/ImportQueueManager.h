#pragma once
#include <deque>
#include <vector>
#include <string>
#include <functional>

// Queues dropped/imported files and drains a few of them per frame, so a large batch drop shows
// real, animated progress instead of one big synchronous stall.
//
// This is deliberately NOT a background OS thread. OpenGL resource creation (texture upload,
// vertex buffer creation - which is most of what importing a model or texture actually does) is
// only valid on whichever thread currently holds the GL context, and this engine never sets up a
// second, shared GL context for a worker thread to safely upload from. Building that (a shared
// context, cross-thread sync, marshaling GL calls back to the main thread) is a real, separate
// piece of infrastructure - so this queue gets the visible behavior (a progress bar, "Cancel
// Remaining", multi-file batches that don't freeze the UI) by spreading synchronous work across
// frames instead, which is safe with zero new GL-threading risk.
class ImportQueueManager {
public:
    using ImportFn = std::function<void(const std::string& path)>;

    // Adds files to the queue and logs a one-line breakdown of what's about to import, by
    // resolved type (e.g. "Queued 3 textures and 1 model for import...") - the closest honest
    // equivalent to a hover-time tooltip, since GLFW only reports the final drop, never drag-in-
    // progress/hover position (there is no pre-drop hover event to attach a tooltip to at all).
    void Enqueue(const std::vector<std::string>& paths);

    // Call once per frame. Pops up to `perFrame` queued paths and invokes `importOne` for each -
    // kept small and constant so even a huge batch never stalls the editor for more than a
    // fraction of a frame, while still finishing in a small, predictable number of frames.
    void Update(const ImportFn& importOne, int perFrame = 2);

    bool IsActive() const { return !m_Pending.empty(); }
    // Drops every not-yet-processed file without importing it; files already drained this
    // Update() are unaffected (there's no partial rollback of an import that already ran).
    void CancelRemaining();

    // Draws the compact, non-modal progress window (bottom-right) while a batch is in flight:
    // current file, an X/N counter, an ImGui::ProgressBar, and the Cancel Remaining button.
    // No-op when IsActive() is false.
    void DrawProgressUI();

private:
    std::deque<std::string> m_Pending;
    int m_TotalInBatch = 0;
    int m_ProcessedInBatch = 0;
    std::string m_LastProcessed;
};
