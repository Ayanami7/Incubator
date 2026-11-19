#include "demo_app.h"

#include <imgui.h>

#include <utility>

#include "showcase.h"

namespace Incubator::Demo
{
    DemoApp::DemoApp(App::SessionOptions options, std::unique_ptr<FileIO::FileIO> fileIO)
        : session_("DemoApp", options, std::move(fileIO))
    {}

    void DemoApp::sceneViewport(const char* title, bool* open)
    {
        session_.requireOpen();
        if (open && !*open)
            return;
        if (ImGui::Begin(title, open, ImGuiWindowFlags_NoDocking))
            session_.drawSceneImage();
        ImGui::End();
    }

    void DemoApp::run()
    {
        run(makeCallbacks());
    }

    void DemoApp::run(Callbacks callbacks)
    {
        session_.run(*this, callbacks);
    }
}  // namespace Incubator::Demo
