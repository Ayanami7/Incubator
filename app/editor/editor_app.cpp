#include "editor_app.h"

#include <imgui.h>

#include <utility>

#include "editor_scene.h"

namespace Incubator::Editor
{
    EditorApp::EditorApp(App::SessionOptions options, std::unique_ptr<FileIO::FileIO> fileIO)
        : session_("EditorApp", options, std::move(fileIO))
    {}

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
