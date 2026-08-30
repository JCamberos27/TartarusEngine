#include "FileDialog.h"
#include <windows.h>
#include <commdlg.h>
#include <filesystem>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace {
// Where the next dialog opens. Seeded once with the project folder (SetDefaultDirectory), then
// updated to the folder of whatever file was last picked — so Import doesn't keep dumping the
// user in build/Release/ and instead remembers where they were last working (#15 P4).
std::string s_lastDir;

void RememberDirOf(const char* pickedPath) {
    std::error_code ec;
    std::filesystem::path p(pickedPath);
    std::filesystem::path dir = p.parent_path();
    if (!dir.empty() && std::filesystem::is_directory(dir, ec) && !ec) {
        s_lastDir = dir.string();
    }
}
}

void FileDialog::SetDefaultDirectory(const std::string& dir) {
    std::error_code ec;
    if (s_lastDir.empty() && !dir.empty() && std::filesystem::is_directory(dir, ec) && !ec) {
        s_lastDir = dir;
    }
}

std::string FileDialog::OpenFile(const char* filter, GLFWwindow* owner) {
    char fileBuffer[MAX_PATH] = {0};

    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = owner ? glfwGetWin32Window(owner) : nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = s_lastDir.empty() ? nullptr : s_lastDir.c_str();
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        RememberDirOf(fileBuffer);
        return std::string(fileBuffer);
    }
    return "";
}

std::string FileDialog::SaveFile(const char* filter, const char* defaultExt, GLFWwindow* owner) {
    char fileBuffer[MAX_PATH] = {0};

    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = owner ? glfwGetWin32Window(owner) : nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defaultExt;
    ofn.lpstrInitialDir = s_lastDir.empty() ? nullptr : s_lastDir.c_str();
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;

    if (GetSaveFileNameA(&ofn)) {
        RememberDirOf(fileBuffer);
        return std::string(fileBuffer);
    }
    return "";
}
