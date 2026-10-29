#pragma once

#include <imgui.h>

#include <memory>
#include <string>

#include "app/common/lighting_controls.h"
#include "app/common/orbit_camera.h"
#include "demo_app.h"
#include "runtime/model/model_manager.h"
#include "runtime/scene/scene_file.h"
#include "runtime/scene/scene_rebuild.h"

namespace Incubator::Demo
{
    /// @brief 展示启动时读取的静态场景，仅保留相机、光照和阴影预览操作。
    class Showcase
    {
    public:
        void initialize(DemoApp& app, std::string_view path = "assets/scenes/shadows.json")
        {
            ImGui::GetIO().IniFilename = nullptr;
            models_ = std::make_unique<Model::ModelManager>(app.renderer());
            auto built = Scene::rebuildScene(Scene::readScene(app.fileIO(), path),
                                             [this](const auto& source) { return models_->resolve(source); });
            initialCamera_ = built.scene->camera();
            app.setScene(std::move(built.scene));
            app.clearColor() = built.clearColor;
            app.shadowSettings().enabled = true;
            orbit_.synchronize(initialCamera_);
        }

        void layout(DemoApp& app)
        {
            const auto* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            app.sceneViewport("Scene");
        }

        void input(DemoApp& app, Render::FrameExtent, const App::UiInput& input)
        {
            auto camera = app.scene().camera();
            if (orbit_.apply(camera, input, app.viewportRegion().size))
                app.scene().setCamera(camera);
        }
        void cancelInteraction()
        {
            orbit_.cancel();
        }
        /// @brief 恢复启动场景中的相机位姿及透视配置。
        void resetCamera(DemoApp& app)
        {
            app.scene().setCamera(initialCamera_);
            orbit_.synchronize(initialCamera_);
        }

        void ui(DemoApp& app)
        {
            const auto* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos({viewport->WorkPos.x + 12, viewport->WorkPos.y + ImGui::GetFontSize() * 2},
                                    ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize({std::min(ImGui::GetFontSize() * 16, viewport->WorkSize.x * 0.45f), 0},
                                     ImGuiCond_Always);
            ImGui::SetNextWindowSizeConstraints({0, 0}, {viewport->WorkSize.x * 0.5f, viewport->WorkSize.y * 0.85f});
            if (ImGui::Begin("Demo controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking))
            {
                if (ImGui::Button("Reset camera"))
                    resetCamera(app);
                ImGui::TextDisabled("Mouse controls (?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Right drag: orbit. Middle drag: pan. Wheel: zoom.");
                if (ImGui::CollapsingHeader("Lighting"))
                    App::lightingControls(app.scene());
                ImGui::Checkbox("Shadows", &app.shadowSettings().enabled);
                if (ImGui::CollapsingHeader("Shadow bias"))
                {
                    ImGui::TextWrapped("Receiver depth bias");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##depthBias", &app.shadowSettings().depthBias, 0, 0.01f, "%.4f");
                }
            }
            ImGui::End();
        }

    private:
        std::unique_ptr<Model::ModelManager> models_;
        Scene::Camera initialCamera_;
        App::OrbitCamera orbit_;
    };

    inline Callbacks makeCallbacks(std::string startupScene = "assets/scenes/shadows.json")
    {
        auto showcase = std::make_shared<Showcase>();
        return {
            [showcase, startupScene = std::move(startupScene)](auto& app) { showcase->initialize(app, startupScene); },
            {},
            [showcase](auto& app) { showcase->ui(app); },
            [showcase](auto&) { showcase->cancelInteraction(); },
            [showcase](auto& app, auto extent, const auto& input) { showcase->input(app, extent, input); },
            [showcase](auto& app) { showcase->layout(app); }};
    }
}  // namespace Incubator::Demo
