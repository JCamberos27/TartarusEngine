#include "FileDialog.h"
#include <windows.h>
#include <commdlg.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

std::string FileDialog::OpenFile(const char* filter, GLFWwindow* owner) {
    char fileBuffer[MAX_PATH] = {0};

    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = owner ? glfwGetWin32Window(owner) : nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
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
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;

    if (GetSaveFileNameA(&ofn)) {
        return std::string(fileBuffer);
    }
    return "";
}
