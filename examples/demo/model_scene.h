#pragma once

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "demo_app.h"
#include "model_cache.h"
#include "orbit_camera.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/model/prepared_model.h"
#include "runtime/scene/scene_file.h"
#include "runtime/scene/scene_rebuild.h"
#include "workspace.h"

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
        changed |= ImGui::ColorEdit3("Color (linear)", &lighting.color.x,
                                     ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
        changed |= ImGui::SliderFloat("Intensity", &lighting.intensity, 0, 4, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        changed |= ImGui::SliderFloat("Ambient", &lighting.ambient, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (changed)
            scene.setLighting(lighting);
    }

    /// @brief 实现 Demo 中的模型场景交互和 UI。
    /// @note 此类只保存使用方状态；实体 Transform、Camera 和光照值由 Runtime Scene 持有。
    class ModelScene
    {
    public:
        void initialize(Demo::DemoApp& app, std::string_view path = "assets/scenes/mixed.json")
        {
            cache_ = std::make_unique<ModelCache>(app.renderer());
            openScene(app, path);
        }

        void update(Demo::DemoApp&, const Demo::FrameTime& time)
        {
            deltaSeconds_ = time.deltaSeconds();
        }

        Scene::EntityId createEntity(Demo::DemoApp& app)
        {
            selected_ = app.scene().createEntity(presetModel(useObj_), initialTransform(useObj_, 0));
            return selected_;
        }

        void selectModel(bool useObj)
        {
            useObj_ = useObj;
        }

        bool save(Demo::DemoApp& app, std::string_view path)
        {
            try
            {
                std::string target(path);
                Scene::writeScene(app.fileIO(), path, Scene::captureScene(app.scene(), app.clearColor()));
                messageError_ = false;
                message_ = "Saved: " + std::string(path);
                currentFile_ = std::move(target);
                Log::Registry::get("demo").info("Saved scene '{}' ({} entities)", logPath(currentFile_),
                                                app.scene().entities().size());
                return true;
            }
            catch (const Error::Exception& error)
            {
                messageError_ = true;
                message_ = error.message();
                Log::Registry::get("demo").error("Failed to save scene '{}': {}", path, error.message());
                return false;
            }
        }

        bool load(Demo::DemoApp& app, std::string_view path)
        {
            try
            {
                openScene(app, path);
                return true;
            }
            catch (const Error::Exception& error)
            {
                messageError_ = true;
                message_ = error.message();
                Log::Registry::get("demo").error("Failed to load scene '{}': {}", path, error.message());
                return false;
            }
        }

        /// @brief 保存到当前文件；没有文件关联时进入另存确认。
        bool save(Demo::DemoApp& app)
        {
            if (currentFile_.empty())
            {
                beginSaveAs();
                return false;
            }
            return save(app, currentFile_);
        }
        void beginSaveAs()
        {
            fileAction_ = FileAction::SaveAs;
        }
        void beginOpen()
        {
            fileAction_ = FileAction::Open;
        }
        void cancelFileAction()
        {
            fileAction_ = FileAction::None;
        }
        void operationPath(std::string_view path)
        {
            scenePath_ = pathBuffer(path);
        }
        bool confirmFileAction(Demo::DemoApp& app)
        {
            const std::string path(scenePath_.data());
            const bool success = fileAction_ == FileAction::Open     ? load(app, path)
                                 : fileAction_ == FileAction::SaveAs ? save(app, path)
                                                                     : false;
            if (success)
                fileAction_ = FileAction::None;
            return success;
        }
        void layout(Demo::DemoApp& app)
        {
            workspace_.begin();
            app.sceneViewport("Scene", &viewportOpen_);
        }
        void restoreLayout()
        {
            workspace_.reset();
        }

        const std::string& message() const { return message_; }
        /// @brief 获取当前场景文件面板中的 UTF-8 路径。
        std::string_view scenePath() const
        {
            return currentFile_;
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
            const auto& region = app.viewportRegion();
            const auto logicalSize = region.size.x > 0 && region.size.y > 0
                                         ? region.size
                                         : Input::PointerValue{double(width), double(height)};
            if (orbit_.apply(camera, input, logicalSize))
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
        /// @note 按需准备展示模型，并在当前会话内复用。
        void switchScene(Demo::DemoApp& app, bool useObj)
        {
            const auto model = presetModel(useObj);
            auto scene = std::make_unique<Scene::Scene>();
            scene->setCamera(Scene::Camera({0, 0, 6}));
            scene->setLighting({{-0.6f, -0.8f, -1}, {1, 0.95f, 0.9f}, 1, 0.15f});
            const float offset = useObj ? 1.65f : 1.2f;
            selected_ = scene->createEntity(model, initialTransform(useObj, -offset));
            scene->createEntity(model, initialTransform(useObj, offset));
            app.setScene(std::move(scene));
            useObj_ = useObj;
            alternateCamera_ = false;
            orbit_.synchronize(app.scene().camera());
        }

        void ui(Demo::DemoApp& app)
        {
            bool saveCurrent = false;
            bool confirm = false;
            ImGui::Begin("Files");
            ImGui::TextWrapped("Current file: %s", currentFile_.empty() ? "(unsaved)" : currentFile_.c_str());
            if (ImGui::Button("Save"))
                saveCurrent = true;
            ImGui::SameLine();
            if (ImGui::Button("Save As"))
                beginSaveAs();
            ImGui::SameLine();
            if (ImGui::Button("Open"))
                beginOpen();
            ImGui::SameLine();
            if (ImGui::Button("Restore layout"))
                restoreLayout();
            ImGui::SameLine();
            ImGui::Checkbox("Scene viewport", &viewportOpen_);
            if (fileAction_ != FileAction::None)
            {
                ImGui::InputText("Target path", scenePath_.data(), scenePath_.size());
                if (ImGui::Button(fileAction_ == FileAction::Open ? "Open target" : "Save target"))
                    confirm = true;
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    cancelFileAction();
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            int modelIndex = useObj_ ? 1 : 0;
            if (ImGui::Combo("New model", &modelIndex, "Avocado\0Teapot\0")) useObj_ = modelIndex == 1;
            ImGui::SameLine();
            if (ImGui::Button("Create entity"))
                createEntity(app);
            ImGui::SameLine();
            if (ImGui::Button("Clear scene"))
                clear(app);
            ImGui::SameLine();
            if (ImGui::Button("Reset scene"))
                switchScene(app, useObj_);
            ImGui::SameLine();
            if (ImGui::Button(useObj_ ? "Switch to glTF" : "Switch to OBJ"))
                switchScene(app, !useObj_);
            ImGui::SameLine();
            if (ImGui::Button("Exit"))
                app.requestExit();
            ImGui::End();
            ImGui::Begin("Properties");
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.42f);
            ImGui::ColorEdit3("Clear color", app.clearColor().data(), ImGuiColorEditFlags_NoInputs);
            if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
                lightingControls(app.scene());
            if (ImGui::Button("Replace camera"))
                replaceCamera(app);
            if (ImGui::Button("Reset camera"))
                resetCamera(app);
            ImGui::SameLine();
            if (ImGui::Button("Focus selected"))
            {
                focusSelected(app, extent_);
            }
            ImGui::TextWrapped("Camera distance alternates between 6 and 8.");
            ImGui::PopItemWidth();
            ImGui::End();
            ImGui::Begin("Objects");
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
            ImGui::End();
            ImGui::Begin("Properties");
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.42f);
            if (const auto* entity = app.scene().find(selected_))
            {
                auto transform = entity->transform;
                auto position = transform.position();
                auto scale = transform.scale();
                auto degrees = glm::degrees(glm::eulerAngles(transform.rotation()));
                bool visible = entity->visible;
                if (ImGui::Checkbox("Visible", &visible))
                    app.scene().setVisible(selected_, visible);
                bool changed = false;
                if (ImGui::DragFloat3("Position", &position.x, 0.05f))
                {
                    transform.setPosition(position);
                    changed = true;
                }
                if (ImGui::SliderFloat3("Rotation", &degrees.x, -180, 180, "%.0f", ImGuiSliderFlags_AlwaysClamp))
                {
                    transform.setRotation(glm::quat(glm::radians(degrees)));
                    changed = true;
                }
                if (ImGui::DragFloat3("Scale", &scale.x, 0.05f))
                {
                    transform.setScale(scale);
                    changed = true;
                }
                if (changed)
                    app.scene().setTransform(selected_, transform);
                if (ImGui::Button("Delete selected"))
                    deleteSelected(app);
            }
            ImGui::PopItemWidth();
            ImGui::End();
            ImGui::Begin("Status");
            ImGui::Text("Scene: %u x %u | Update: %.2f ms", app.sceneExtent().width, app.sceneExtent().height,
                        deltaSeconds_ * 1000);
            if (messageError_)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.35f, 1));
            ImGui::TextWrapped("%s", message_.c_str());
            if (messageError_)
                ImGui::PopStyleColor();
            ImGui::TextWrapped(
                "Saving a deployed copy does not update source assets. Build cleanup deletes it; asset redeployment "
                "may overwrite it.");
            ImGui::End();
            if (saveCurrent)
                save(app);
            if (confirm)
                confirmFileAction(app);
        }

    private:
        static std::string logPath(std::string_view path)
        {
            std::error_code error;
            const auto absolute = std::filesystem::absolute(Utils::Utf8::toFsPath(path), error);
            return error ? std::string(path) : Utils::Utf8::toUtf8String(absolute.lexically_normal());
        }

        static std::vector<char> pathBuffer(std::string_view path)
        {
            std::vector<char> buffer(std::max(size_t{1024}, path.size() + 1), '\0');
            std::copy(path.begin(), path.end(), buffer.begin());
            return buffer;
        }

        void openScene(Demo::DemoApp& app, std::string_view path)
        {
            auto buffer = pathBuffer(path);
            const auto description = Scene::readScene(app.fileIO(), path);
            auto rebuilt = Scene::rebuildScene(description, [this](const auto& source) { return cache_->resolve(source); });
            auto message = "Loaded: " + std::string(path);
            app.setScene(std::move(rebuilt.scene));
            app.clearColor() = rebuilt.clearColor;
            selected_ = {};
            orbit_.synchronize(app.scene().camera());
            scenePath_ = std::move(buffer);
            currentFile_ = scenePath_.data();
            messageError_ = false;
            message_ = std::move(message);
            Log::Registry::get("demo").info("Loaded scene '{}' ({} entities)", logPath(scenePath_.data()),
                                            app.scene().entities().size());
        }

        std::shared_ptr<const Model::PreparedModel> presetModel(bool useObj)
        {
            return cache_->resolve(Model::ModelReference(
                useObj ? "models/utah_teapot/utah_teapot.obj" : "models/avocado/glTF/Avocado.gltf", PREVIEW_OPTIONS));
        }

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

        enum class FileAction
        {
            None,
            Open,
            SaveAs
        };
        FileAction fileAction_ = FileAction::None;
        std::string currentFile_;
        Workspace workspace_;
        bool viewportOpen_ = true;
        std::unique_ptr<ModelCache> cache_;
        Scene::EntityId selected_;
        bool useObj_ = false;
        bool alternateCamera_ = false;
        std::vector<char> scenePath_ = pathBuffer("");
        std::string message_;
        bool messageError_ = false;
        double deltaSeconds_ = 0;
        OrbitCamera orbit_;
        Render::FrameExtent extent_{0, 0};
    };

    inline Demo::Callbacks makeCallbacks(std::string startupScene = "assets/scenes/mixed.json")
    {
        auto demo = std::make_shared<ModelScene>();
        return {[demo, startupScene = std::move(startupScene)](auto& app) { demo->initialize(app, startupScene); },
                [demo](auto& app, const auto& time) { demo->update(app, time); },
                [demo](auto& app) { demo->ui(app); },
                [demo](auto&) { demo->cancelInteraction(); },
                [demo](auto& app, auto extent, const auto& input) { demo->input(app, extent, input); },
                [demo](auto& app) { demo->layout(app); }};
    }
}  // namespace Incubator::Demo
