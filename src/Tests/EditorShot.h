#pragma once

#include <string>
#include <vector>

struct LaunchOptions;
class EditorLayer;
class Window;
class World;

// --editor-shot <dir>: a screenshot of each editor view in turn. Each view gets kFrames frames - set up halfway through,
// captured at the end - driven from main's frame loop.
class EditorShot {
public:
    explicit EditorShot(const LaunchOptions& opt);

    bool Active() const { return !m_Dir.empty(); }

    // Before the editor UI is built: sets the next view up a second ahead of its capture.
    void BeforeUi(EditorLayer& editor, World& world);
    // Inside the UI frame: the recoil curve editor's preview window (--editor-shot-curves).
    void DrawPreviewWindows();
    // After the editor has drawn: the whole window once the view has settled; the last one sets exitApproved and closes.
    void AfterFrame(Window& window, bool& exitApproved);
    // The run's result for its exit code: every view captured and written, and the recoil preview loaded.
    bool Succeeded() const { return m_Failures == 0 && m_Written == (int)m_Views.size(); }

private:
    static constexpr int kFrames = 150;
    std::string m_Dir;
    bool m_Curves = false;
    std::vector<const char*> m_Views;
    int m_Frame = 0;
    int m_Written = 0;  // views written to disk
    bool m_ViewLit = true; // the last BeforeUi saw the views Shaded and lit (HarnessView)
    int m_Failures = 0; // a PNG that could not be written, or the recoil preview that did not load
};
