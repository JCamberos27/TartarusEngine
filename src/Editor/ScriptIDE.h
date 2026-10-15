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
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
