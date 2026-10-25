#pragma once

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>

#include "demo_app.h"
#include "orbit_camera.h"
#include "runtime/model/prepared_model.h"

namespace Incubator::Demo
{
    /// @brief 编辑 Scene 当前光照；角度及颜色只在本次 UI 构建期间暂存。
    inline void lightingControls(Scene::Scene& scene)
    {
        auto lighting = scene.lighting();
        const double length =
            std::hypot(double(lighting.direction.x), double(lighting.direction.y), double(lighting.direction.z));
        const auto direction = glm::dvec3(lighting.direction) / length;
        float yaw = glm::degrees(float(std::atan2(direction.x, -direction.z)));
        float pitch = glm::degrees(float(std::asin(std::clamp(direction.y, -1.0, 1.0))));
        bool directionChanged = ImGui::SliderFloat("Light yaw", &yaw, -180, 180, "%.0f", ImGuiSliderFlags_AlwaysClamp);
        directionChanged |= ImGui::SliderFloat("Light pitch", &pitch, -89, 89, "%.0f", ImGuiSliderFlags_AlwaysClamp);
        if (directionChanged)
        {
            const float y = glm::radians(yaw), p = glm::radians(pitch);
            lighting.direction = {std::cos(p) * std::sin(y), std::sin(p), -std::cos(p) * std::cos(y)};
        }
        bool changed = directionChanged;
        changed |= ImGui::ColorEdit3("Light color (linear)", &lighting.color.x, ImGuiColorEditFlags_Float);
        changed |=
            ImGui::SliderFloat("Light intensity", &lighting.intensity, 0, 4, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        changed |= ImGui::SliderFloat("Ambient", &lighting.ambient, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (changed)
            scene.setLighting(lighting);
    }

    /// @brief 实现 Demo 中的模型场景交互和 UI。
    /// @note 此类只保存使用方状态；实体 Transform、Camera 和光照值由 Runtime Scene 持有。
    class ModelScene
    {
    public:
        void initialize(Demo::DemoApp& app)
        {
            models_[0] = Model::prepareModel(app.renderer(), "models/avocado/glTF/Avocado.gltf", PREVIEW_OPTIONS);
            switchScene(app, false);
        }

        void update(Demo::DemoApp& app, const Demo::FrameTime& time)
        {
            deltaSeconds_ = time.deltaSeconds();
            // 初始化阶段释放 glTF 的 CPU 资源后，再于后续更新中准备 OBJ。
            if (firstUpdate_)
                firstUpdate_ = false;
            else if (!models_[1])
                models_[1] = Model::prepareModel(app.renderer(), "models/utah_teapot/utah_teapot.obj", PREVIEW_OPTIONS);
        }

        Scene::EntityId createEntity(Demo::DemoApp& app)
        {
            selected_ = app.scene().createEntity(models_[useObj_ ? 1 : 0], initialTransform(useObj_, 0));
            return selected_;
        }

        Scene::EntityId selected() const
        {
            return selected_;
        }
        void select(Scene::EntityId id)
        {
            selected_ = id;
        }

        void deleteSelected(Demo::DemoApp& app)
        {
            app.scene().destroyEntity(selected_);
            selected_ = {};
        }

        void clear(Demo::DemoApp& app)
        {
            app.scene().clear();
            selected_ = {};
        }

        void replaceCamera(Demo::DemoApp& app)
        {
            alternateCamera_ = !alternateCamera_;
            app.scene().setCamera(Scene::Camera({0, 0, alternateCamera_ ? 8.0f : 6.0f}));
            orbit_.synchronize(app.scene().camera());
        }

        void cancelInteraction()
        {
            orbit_.cancel();
            extent_ = {0, 0};
        }

        bool focusSelected(Demo::DemoApp& app, Render::FrameExtent extent)
        {
            const auto* entity = app.scene().find(selected_);
            if (!entity || !entity->model || !entity->model->bounds())
                return false;
            const auto bounds = entity->model->bounds()->transformed(entity->transform.matrix());
            auto camera = app.scene().camera();
            if (!orbit_.focus(camera, bounds, extent))
                return false;
            app.scene().setCamera(camera);
            return true;
        }

        void input(Demo::DemoApp& app, Render::FrameExtent extent, const UiInput& input)
        {
            extent_ = extent;
            int width, height;
            app.window().getWindowSize(width, height);
            auto camera = app.scene().camera();
            if (orbit_.apply(camera, input, {double(width), double(height)}))
                app.scene().setCamera(camera);
            if (input.focusPressed && !input.captureKeyboard)
                focusSelected(app, extent);
        }

        void resetCamera(Demo::DemoApp& app)
        {
            app.scene().setCamera(Scene::Camera({0, 0, 6}));
            orbit_.synchronize(app.scene().camera());
            alternateCamera_ = false;
        }

        /// @brief 替换当前模型展示场景。
        /// @param app 当前 Demo 应用。
        /// @param useObj 是否使用 OBJ 模型。
        /// @note 仅在目标模型准备完成后调用；后续更新完成后，UI 才会启用 OBJ 切换。
        void switchScene(Demo::DemoApp& app, bool useObj)
        {
            auto scene = std::make_unique<Scene::Scene>();
            scene->setCamera(Scene::Camera({0, 0, 6}));
            scene->setLighting({{-0.6f, -0.8f, -1}, {1, 0.95f, 0.9f}, 1, 0.15f});
            const float offset = useObj ? 1.65f : 1.2f;
            selected_ = scene->createEntity(models_[useObj ? 1 : 0], initialTransform(useObj, -offset));
            scene->createEntity(models_[useObj ? 1 : 0], initialTransform(useObj, offset));
            app.setScene(std::move(scene));
            useObj_ = useObj;
            alternateCamera_ = false;
            orbit_.synchronize(app.scene().camera());
        }

        void ui(Demo::DemoApp& app)
        {
            ImGui::SetNextWindowPos({12, 12}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize({350, 600}, ImGuiCond_FirstUseEver);
            ImGui::Begin("Incubator Scene Runtime");
            ImGui::Text("Scene: %s | Update: %.2f ms", useObj_ ? "OBJ" : "glTF", deltaSeconds_ * 1000);
            ImGui::ColorEdit3("Clear color", app.clearColor().data());
            if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
                lightingControls(app.scene());
            if (ImGui::Button("Create entity"))
                createEntity(app);
            ImGui::SameLine();
            if (ImGui::Button("Clear scene"))
                clear(app);
            if (ImGui::Button("Reset scene"))
                switchScene(app, useObj_);
            ImGui::SameLine();
            ImGui::BeginDisabled(!models_[1]);
            if (ImGui::Button(useObj_ ? "Switch to glTF" : "Switch to OBJ"))
                switchScene(app, !useObj_);
            ImGui::EndDisabled();
            if (ImGui::Button("Replace camera"))
                replaceCamera(app);
            if (ImGui::Button("Reset camera"))
                resetCamera(app);
            ImGui::SameLine();
            if (ImGui::Button("Focus selected"))
            {
                focusSelected(app, extent_);
            }
            ImGui::TextUnformatted("Camera distance alternates between 6 and 8.");
            ImGui::Text("Entities: %zu", app.scene().entities().size());
            if (ImGui::BeginListBox("##entities", {-1, 110}))
            {
                for (const auto& entity : app.scene().entities())
                {
                    const auto label = std::to_string(entity.id.value());
                    if (ImGui::Selectable(label.c_str(), selected_ == entity.id))
                        select(entity.id);
                }
                ImGui::EndListBox();
            }
            if (const auto* entity = app.scene().find(selected_))
            {
                auto transform = entity->transform;
                auto position = transform.position();
                auto scale = transform.scale() / glm::vec3(initialScale(useObj_));
                auto degrees = glm::degrees(glm::eulerAngles(transform.rotation()));
                bool visible = entity->visible;
                if (ImGui::Checkbox("Visible", &visible))
                    app.scene().setVisible(selected_, visible);
                bool changed = false;
                if (ImGui::SliderFloat3("Position", &position.x, -3, 3, "%.2f", ImGuiSliderFlags_AlwaysClamp))
                {
                    transform.setPosition(position);
                    changed = true;
                }
                if (ImGui::SliderFloat3("Rotation", &degrees.x, -180, 180, "%.0f", ImGuiSliderFlags_AlwaysClamp))
                {
                    transform.setRotation(glm::quat(glm::radians(degrees)));
                    changed = true;
                }
                if (ImGui::SliderFloat3("Scale multiplier", &scale.x, 0.1f, 3, "%.2f", ImGuiSliderFlags_AlwaysClamp))
                {
                    transform.setScale(scale * initialScale(useObj_));
                    changed = true;
                }
                if (changed)
                    app.scene().setTransform(selected_, transform);
                if (ImGui::Button("Delete selected"))
                    deleteSelected(app);
            }
            if (ImGui::Button("Exit"))
                app.requestExit();
            ImGui::End();
        }

    private:
        static constexpr Model::PrepareOptions PREVIEW_OPTIONS{
            Render::TextureMipMode::FullChain,
            {Render::TextureFilter::Linear, Render::TextureFilter::Linear, Render::MipFilter::Linear},
            Render::ShadingMode::Lambert};

        static float initialScale(bool useObj)
        {
            return useObj ? 0.075f : 24.0f;
        }

        static Scene::Transform initialTransform(bool useObj, float x)
        {
            if (useObj)
                return Scene::Transform({x, 0, 0}, {1, 0, 0, 0}, glm::vec3(initialScale(true)));
            return Scene::Transform({x, -0.78f, 0}, {1, 0, 0, 0}, glm::vec3(initialScale(false)));
        }

        std::array<std::shared_ptr<const Model::PreparedModel>, 2> models_;
        Scene::EntityId selected_;
        bool useObj_ = false;
        bool alternateCamera_ = false;
        bool firstUpdate_ = true;
        double deltaSeconds_ = 0;
        OrbitCamera orbit_;
        Render::FrameExtent extent_{0, 0};
    };

    inline Demo::Callbacks makeCallbacks()
    {
        auto demo = std::make_shared<ModelScene>();
        return {[demo](auto& app) { demo->initialize(app); },
                [demo](auto& app, const auto& time) { demo->update(app, time); }, [demo](auto& app) { demo->ui(app); },
                [demo](auto&) { demo->cancelInteraction(); },
                [demo](auto& app, auto extent, const auto& input) { demo->input(app, extent, input); }};
    }
}  // namespace Incubator::Demo
