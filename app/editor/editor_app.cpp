#include "editor_app.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#include <imgui.h>

#include <array>
#include <utility>

#include "editor_scene.h"
#include "runtime/core/error/error.h"

namespace Incubator::Editor
{
    EditorApp::EditorApp(App::SessionOptions options, std::unique_ptr<FileIO::FileIO> fileIO)
        : session_("EditorApp", options, std::move(fileIO))
    {}

    std::optional<std::filesystem::path> EditorApp::pickExternalModelFile()
    {
        std::array<wchar_t, 32768> path{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = glfwGetWin32Window(static_cast<GLFWwindow*>(window().getNativeHandle()));
        dialog.lpstrFilter = L"Model files (*.gltf;*.glb;*.obj)\0*.gltf;*.glb;*.obj\0";
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = static_cast<DWORD>(path.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (GetOpenFileNameW(&dialog))
            return std::filesystem::path(path.data());
        if (const DWORD error = CommDlgExtendedError())
            throw Error::Exception(Error::Code::IO, fmt::format("Cannot open model file picker (error: {})", error));
        return std::nullopt;
    }

    std::optional<std::filesystem::path> EditorApp::pickScreenshotSavePath()
    {
        std::array<wchar_t, 32768> path{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = glfwGetWin32Window(static_cast<GLFWwindow*>(window().getNativeHandle()));
        dialog.lpstrFilter = L"PNG image (*.png)\0*.png\0";
        dialog.lpstrDefExt = L"png";
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = static_cast<DWORD>(path.size());
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (GetSaveFileNameW(&dialog))
            return std::filesystem::path(path.data());
        if (const DWORD error = CommDlgExtendedError())
            throw Error::Exception(Error::Code::IO, fmt::format("Cannot open screenshot save picker (error: {})", error));
        return std::nullopt;
    }

    void EditorApp::sceneViewport(const char* title, bool* open)
    {
        session_.requireOpen();
        if (open && !*open)
            return;
        ImGui::SetNextWindowPos({380, 12}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({800, 600}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title, open))
            session_.drawSceneImage();
        ImGui::End();
    }

    void EditorApp::run()
    {
        run(makeCallbacks());
    }

    void EditorApp::run(Callbacks callbacks)
    {
        session_.run(*this, callbacks);
    }
}  // namespace Incubator::Editor
