#pragma once
#include <memory>
#include <string>

// Native dockable editor for the engine's C++ sources, project shaders and .tescript files. Saving a
// reloadable module source rebuilds it in the background (GameModuleBuild) for hot reload.
class ScriptIDE {
public:
    ScriptIDE();
    ~ScriptIDE();
    void Open(const std::string& path,int line=0,int column=1);
    void Show();
    void Draw(unsigned int sceneDockId=0);
    bool HasUnsavedFiles() const;
    bool OwnsKeyboard() const;
    bool SaveAll();
    void DiscardAll();
    void CancelPrompts(); // drops a pending close / reload, so its "Unsaved script" prompt isn't reopened
    // Where the IDE keeps session.json (open tabs, unsaved buffers, theme, panel sizes): the
    // project's Library/IDE unless set before the IDE is created. --editor-tests points it at its
    // scratch user folder, so a test run never reads or overwrites the user's session.
    static void SetSessionDirectory(const std::string& dir);
    static std::string SessionFile();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
