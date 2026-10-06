#pragma once
#include <memory>
#include <string>

// Native dockable editor; compiler analysis runs in a separate Roslyn process.
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
