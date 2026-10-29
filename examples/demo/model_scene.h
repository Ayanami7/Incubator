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
#include "orbit_camera.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/model/model_manager.h"
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
        ImGui::TextUnformatted("Light yaw");
        bool directionChanged = ImGui::SliderFloat("##lightYaw", &yaw, -180, 180, "%.0f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::TextUnformatted("Light pitch");
        directionChanged |= ImGui::SliderFloat("##lightPitch", &pitch, -89, 89, "%.0f", ImGuiSliderFlags_AlwaysClamp);
        if (directionChanged)
        {
            const float y = glm::radians(yaw), p = glm::radians(pitch);
            lighting.direction = {std::cos(p) * std::sin(y), std::sin(p), -std::cos(p) * std::cos(y)};
        }
        bool changed = directionChanged;
        ImGui::TextUnformatted("Color (linear)");
        changed |= ImGui::ColorEdit3("##lightColor", &lighting.color.x,
                                     ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
        ImGui::TextUnformatted("Intensity");
        changed |= ImGui::SliderFloat("##intensity", &lighting.intensity, 0, 4, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::TextUnformatted("Ambient");
        changed |= ImGui::SliderFloat("##ambient", &lighting.ambient, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
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
            cache_ = std::make_unique<Model::ModelManager>(app.renderer());
            openScene(app, path);
        }

        Scene::EntityId createEntity(Demo::DemoApp& app)
        {
            selected_ = app.scene().createEntity(creationModel(useObj_), initialTransform(useObj_));
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
                if (!dynamic_cast<const Model::LoadError*>(&error))
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
            menu(app);
            statusBar();
            workspace_.begin();
            app.sceneViewport("Scene", &viewportOpen_);
        }
        void restoreLayout()
        {
            workspace_.reset();
        }

        const std::string& message() const
        {
            return message_;
        }
        /// @brief 获取当前关联场景文件的 UTF-8 路径。
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
        }

        void ui(Demo::DemoApp& app)
        {
            objects(app);
            properties(app);
            bool confirm = false;
            if (fileAction_ != FileAction::None)
            {
                bool open = true;
                ImGui::SetNextWindowSize({ImGui::GetFontSize() * 30, 0}, ImGuiCond_Appearing);
                ImGui::Begin(fileAction_ == FileAction::Open ? "Open scene" : "Save scene as", &open,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
                ImGui::TextWrapped("Current file: %s", currentFile_.empty() ? "(unsaved)" : currentFile_.c_str());
                ImGui::TextUnformatted("Target path");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##path", scenePath_.data(), scenePath_.size());
                if (ImGui::Button(fileAction_ == FileAction::Open ? "Open" : "Save"))
                    confirm = true;
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    open = false;
                ImGui::TextWrapped(
                    "Saving a deployed copy does not update source assets. Build cleanup deletes it; "
                    "asset redeployment may overwrite it.");
                if (messageError_)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.35f, 1));
                    ImGui::TextWrapped("%s", message_.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::End();
                if (!open)
                {
                    cancelFileAction();
                    confirm = false;
                }
            }
            if (saveRequested_)
            {
                saveRequested_ = false;
                save(app);
            }
            if (confirm)
                confirmFileAction(app);
        }

    private:
        void menu(Demo::DemoApp& app)
        {
            if (!ImGui::BeginMainMenuBar())
                return;
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open..."))
                    beginOpen();
                if (ImGui::MenuItem("Save"))
                    saveRequested_ = true;
                if (ImGui::MenuItem("Save As..."))
                    beginSaveAs();
                ImGui::Separator();
                if (ImGui::BeginMenu("About saved files"))
                {
                    ImGui::TextUnformatted("Saving a deployed copy does not update source assets.");
                    ImGui::TextUnformatted("Build cleanup deletes it; asset redeployment may overwrite it.");
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit"))
                    app.requestExit();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View"))
            {
                ImGui::MenuItem("Scene viewport", nullptr, &viewportOpen_);
                if (ImGui::MenuItem("Restore default layout"))
                    restoreLayout();
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        void statusBar()
        {
            const auto flags =
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding);
            if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down,
                                            ImGui::GetFrameHeight(), flags))
            {
                if (messageError_)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.35f, 1));
                ImGui::Text("%s | %s", currentFile_.empty() ? "(unsaved)" : currentFile_.c_str(), message_.c_str());
                if (messageError_)
                    ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35);
                    ImGui::TextWrapped("%s\n%s", currentFile_.c_str(), message_.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        void objects(Demo::DemoApp& app)
        {
            ImGui::Begin("Objects");
            ImGui::TextUnformatted("New model");
            ImGui::SetNextItemWidth(-1);
            int modelIndex = useObj_ ? 1 : 0;
            if (ImGui::Combo("##model", &modelIndex, "Avocado\0Teapot\0"))
                useObj_ = modelIndex == 1;
            if (ImGui::Button("Create object", {-1, 0}))
                createEntity(app);
            if (ImGui::Button("More..."))
                ImGui::OpenPopup("Object actions");
            if (ImGui::BeginPopup("Object actions"))
            {
                if (ImGui::MenuItem("Clear scene", nullptr, false, !app.scene().entities().empty()))
                    clear(app);
                ImGui::EndPopup();
            }
            ImGui::Separator();
            ImGui::Text("Objects: %zu", app.scene().entities().size());
            if (ImGui::BeginListBox("##entities", {-1, -1}))
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
        }

        void properties(Demo::DemoApp& app)
        {
            ImGui::Begin("Properties");
            ImGui::PushItemWidth(-1);
            if (ImGui::CollapsingHeader("Selected object", ImGuiTreeNodeFlags_DefaultOpen))
            {
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
                    ImGui::TextUnformatted("Position");
                    if (ImGui::DragFloat3("##position", &position.x, 0.05f))
                    {
                        transform.setPosition(position);
                        changed = true;
                    }
                    ImGui::TextUnformatted("Rotation");
                    if (ImGui::SliderFloat3("##rotation", &degrees.x, -180, 180, "%.0f", ImGuiSliderFlags_AlwaysClamp))
                    {
                        transform.setRotation(glm::quat(glm::radians(degrees)));
                        changed = true;
                    }
                    ImGui::TextUnformatted("Scale");
                    if (ImGui::DragFloat3("##scale", &scale.x, 0.05f))
                    {
                        transform.setScale(scale);
                        changed = true;
                    }
                    if (changed)
                        app.scene().setTransform(selected_, transform);
                    if (ImGui::Button("Delete selected"))
                        deleteSelected(app);
                }
                else
                    ImGui::TextWrapped("Select an object to edit.");
            }
            if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::Button("Focus selected", {-1, 0}))
                    focusSelected(app, extent_);
                if (ImGui::Button("Reset view", {-1, 0}))
                    resetCamera(app);
            }
            if (ImGui::CollapsingHeader("Scene settings", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::TextUnformatted("Background");
                ImGui::ColorEdit3("##background", app.clearColor().data(), ImGuiColorEditFlags_NoInputs);
                auto& shadows = app.shadowSettings();
                ImGui::Checkbox("Shadows", &shadows.enabled);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Fixed light bounds: center (%g, %g, %g), half extents (%g, %g), depth %g..%g.\n"
                        "1024x1024; the range follows light direction, not the camera.",
                        shadows.center.x, shadows.center.y, shadows.center.z, shadows.halfExtent.x,
                        shadows.halfExtent.y, shadows.nearPlane, shadows.farPlane);
                ImGui::TextUnformatted("Receiver depth bias");
                ImGui::SliderFloat("##shadowBias", &shadows.depthBias, 0, 0.01f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextWrapped("Shadow preview settings are not saved in scene files.");
                ImGui::Separator();
                lightingControls(app.scene());
            }
            ImGui::PopItemWidth();
            ImGui::End();
        }

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
            auto rebuilt =
                Scene::rebuildScene(description, [this](const auto& source) { return cache_->resolve(source); });
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

        std::shared_ptr<const Model::PreparedModel> creationModel(bool useObj)
        {
            return cache_->resolve(Model::ModelReference(
                useObj ? "models/utah_teapot/utah_teapot.obj" : "models/avocado/glTF/Avocado.gltf", PREVIEW_OPTIONS));
        }

        static constexpr Model::PrepareOptions PREVIEW_OPTIONS{
            Render::TextureMipMode::FullChain,
            {Render::TextureFilter::Linear, Render::TextureFilter::Linear, Render::MipFilter::Linear},
            Render::ShadingMode::Lambert};

        static Scene::Transform initialTransform(bool useObj)
        {
            return Scene::Transform({0, useObj ? 0.0f : -0.78f, 0}, {1, 0, 0, 0}, glm::vec3(useObj ? 0.075f : 24.0f));
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
        std::unique_ptr<Model::ModelManager> cache_;
        Scene::EntityId selected_;
        bool useObj_ = false;
        bool saveRequested_ = false;
        std::vector<char> scenePath_ = pathBuffer("");
        std::string message_;
        bool messageError_ = false;
        OrbitCamera orbit_;
        Render::FrameExtent extent_{0, 0};
    };

    inline Demo::Callbacks makeCallbacks(std::string startupScene = "assets/scenes/shadows.json")
    {
        auto demo = std::make_shared<ModelScene>();
        return {[demo, startupScene = std::move(startupScene)](auto& app) {
                    demo->initialize(app, startupScene);
                    app.shadowSettings().enabled = true;
                },
                {},
                [demo](auto& app) { demo->ui(app); },
                [demo](auto&) { demo->cancelInteraction(); },
                [demo](auto& app, auto extent, const auto& input) { demo->input(app, extent, input); },
                [demo](auto& app) { demo->layout(app); }};
    }
}  // namespace Incubator::Demo
