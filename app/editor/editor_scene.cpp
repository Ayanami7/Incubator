#include "editor_scene.h"

#include <imgui.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/common/lighting_controls.h"
#include "app/common/viewport.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/model/prepared_model.h"
#include "runtime/resource/resource_manager.h"
#include "selection_overlay.h"

namespace Incubator::Editor
{
    bool EditorScene::addModel(Editor::EditorApp& app, std::string_view path, const Scene::Transform& transform)
    {
        return operations_.addModel(app.scene(), app.clearColor(), path, transform);
    }

    void EditorScene::initialize(Editor::EditorApp& app, std::string_view path)
    {
        operations_.initialize(app.renderer());
        if (!path.empty())
            load(app, path, true);
        browseDirectory();
    }

    Scene::EntityId EditorScene::createEntity(Editor::EditorApp& app)
    {
        return addModel(app, creationPath(useObj_), initialTransform(useObj_)) ? operations_.selected()
                                                                               : Scene::EntityId{};
    }

    bool EditorScene::browseDirectory(std::string_view path)
    {
        try
        {
            auto entries = Resource::ResourceManager::instance().listDirectory(path);
            assetDirectory_ = path;
            assetEntries_ = std::move(entries);
            assetError_.clear();
            return true;
        }
        catch (const Error::Exception& error)
        {
            assetEntries_.clear();
            assetError_ = "Cannot browse '" + std::string(path) + "': " + error.message();
            Log::Registry::get("editor").error("{}", assetError_);
            return false;
        }
    }

    const std::string& EditorScene::assetDirectory() const
    {
        return assetDirectory_;
    }

    const std::vector<Resource::AssetEntry>& EditorScene::assetEntries() const
    {
        return assetEntries_;
    }

    const std::string& EditorScene::assetError() const
    {
        return assetError_;
    }

    bool EditorScene::parentDirectory()
    {
        const auto slash = assetDirectory_.find_last_of('/');
        return browseDirectory(slash == std::string::npos ? std::string{} : assetDirectory_.substr(0, slash));
    }

    void EditorScene::selectModel(bool useObj)
    {
        useObj_ = useObj;
    }

    bool EditorScene::save(Editor::EditorApp& app, std::string_view path)
    {
        return operations_.save(app.fileIO(), app.scene(), app.clearColor(), path);
    }

    bool EditorScene::load(Editor::EditorApp& app, std::string_view path, bool rethrow)
    {
        auto rebuilt = operations_.load(app.fileIO(), path, rethrow);
        if (!rebuilt)
            return false;
        app.setScene(std::move(rebuilt->scene));
        app.clearColor() = rebuilt->clearColor;
        cancelInteraction(app);
        clearPendingAction();
        orbit_.synchronize(app.scene().camera());
        scenePath_ = pathBuffer(path);
        return true;
    }

    bool EditorScene::save(Editor::EditorApp& app)
    {
        if (operations_.currentFile().empty())
        {
            beginSaveAs();
            return false;
        }
        return save(app, operations_.currentFile());
    }

    void EditorScene::beginSaveAs()
    {
        fileAction_ = FileAction::SaveAs;
    }

    void EditorScene::beginOpen()
    {
        fileAction_ = FileAction::Open;
    }

    void EditorScene::cancelFileAction()
    {
        fileAction_ = FileAction::None;
    }

    void EditorScene::requestOpen()
    {
        if (!operations_.isDirty())
        {
            beginOpen();
            return;
        }
        pendingAction_ = PendingAction::Open;
        pendingSaveAs_ = false;
    }

    void EditorScene::requestClear(Editor::EditorApp& app)
    {
        if (!operations_.isDirty())
        {
            clear(app);
            return;
        }
        pendingAction_ = PendingAction::Clear;
        pendingSaveAs_ = false;
    }

    void EditorScene::requestExit(Editor::EditorApp& app)
    {
        if (!operations_.isDirty())
        {
            app.requestExit();
            return;
        }
        pendingAction_ = PendingAction::Exit;
        pendingSaveAs_ = false;
    }

    void EditorScene::requestClose(Editor::EditorApp& app)
    {
        if (!operations_.isDirty())
            return;
        app.window().setShouldClose(false);
        pendingAction_ = PendingAction::Exit;
        pendingSaveAs_ = false;
    }

    void EditorScene::executePendingAction(Editor::EditorApp& app)
    {
        switch (pendingAction_)
        {
            case PendingAction::Open:
                beginOpen();
                break;
            case PendingAction::Clear:
                clear(app);
                break;
            case PendingAction::Exit:
                app.requestExit();
                break;
            case PendingAction::None:
                break;
        }
    }

    void EditorScene::clearPendingAction() noexcept
    {
        pendingAction_ = PendingAction::None;
        pendingSaveAs_ = false;
    }

    void EditorScene::operationPath(std::string_view path)
    {
        scenePath_ = pathBuffer(path);
    }

    bool EditorScene::confirmFileAction(Editor::EditorApp& app)
    {
        const std::string path(scenePath_.data());
        const bool success = fileAction_ == FileAction::Open     ? load(app, path)
                             : fileAction_ == FileAction::SaveAs ? save(app, path)
                                                                 : false;
        if (success)
            fileAction_ = FileAction::None;
        return success;
    }

    void EditorScene::layout(Editor::EditorApp& app)
    {
        menu(app);
        statusBar();
        workspace_.begin();
        app.sceneViewport("Scene", &viewportOpen_);
    }

    void EditorScene::restoreLayout()
    {
        workspace_.reset();
    }

    std::string EditorScene::statusText() const
    {
        std::string text = operations_.currentFile().empty() ? "(unsaved)" : operations_.currentFile();
        if (operations_.isDirty())
            text += " *";
        text += " | ";
        text += operations_.message();
        return text;
    }

    const std::string& EditorScene::message() const
    {
        return operations_.message();
    }

    std::string_view EditorScene::scenePath() const
    {
        return operations_.currentFile();
    }

    Scene::EntityId EditorScene::selected() const
    {
        return operations_.selected();
    }

    void EditorScene::select(Scene::EntityId id)
    {
        operations_.select(id);
    }

    void EditorScene::deleteSelected(Editor::EditorApp& app)
    {
        operations_.remove(app.scene(), app.clearColor());
        clickCandidate_ = false;
    }

    bool EditorScene::undo(Editor::EditorApp& app)
    {
        if (!operations_.undo(app.scene(), app.clearColor()))
            return false;
        lightingTransactionActive_ = false;
        backgroundTransactionActive_ = false;
        clickCandidate_ = false;
        orbit_.synchronize(app.scene().camera());
        return true;
    }

    bool EditorScene::redo(Editor::EditorApp& app)
    {
        if (!operations_.redo(app.scene(), app.clearColor()))
            return false;
        lightingTransactionActive_ = false;
        backgroundTransactionActive_ = false;
        clickCandidate_ = false;
        orbit_.synchronize(app.scene().camera());
        return true;
    }

    Scene::EntityId EditorScene::duplicateSelected(Editor::EditorApp& app)
    {
        return operations_.duplicate(app.scene(), app.clearColor());
    }

    void EditorScene::clear(Editor::EditorApp& app)
    {
        operations_.clear(app.scene(), app.clearColor());
        clickCandidate_ = false;
    }

    void EditorScene::cancelInteraction(Editor::EditorApp& app)
    {
        orbit_.cancel();
        clickCandidate_ = false;
        extent_ = {0, 0};
        int fbWidth = 0;
        int fbHeight = 0;
        app.window().getFramebufferSize(fbWidth, fbHeight);
        const auto& raw = app.window().input();
        const bool windowUnavailable = !raw.focused() || raw.focusLost() || fbWidth <= 0 || fbHeight <= 0;
        const bool viewportVisible = app.sceneExtent().width > 0 && app.sceneExtent().height > 0;
        if (!raw.focused() || raw.focusLost())
            shortcutSuspended_ = true;
        // 仅因 Scene 视口折叠或关闭触发的取消只重置视口手势，保留 Properties 面板正在进行的属性事务。
        if (windowUnavailable || viewportVisible)
        {
            lightingTransactionActive_ = false;
            backgroundTransactionActive_ = false;
            operations_.cancelTransaction(app.scene(), app.clearColor());
        }
        else if (!operations_.inTransaction())
        {
            lightingTransactionActive_ = false;
            backgroundTransactionActive_ = false;
        }
    }

    bool EditorScene::focusSelected(Editor::EditorApp& app, Render::FrameExtent extent)
    {
        const auto* entity = app.scene().find(operations_.selected());
        if (!entity || !entity->model || !entity->model->bounds())
            return false;
        const auto bounds = entity->model->bounds()->transformed(entity->transform.matrix());
        auto camera = app.scene().camera();
        if (!orbit_.focus(camera, bounds, extent))
            return false;
        app.scene().setCamera(camera);
        return true;
    }

    void EditorScene::input(Editor::EditorApp& app, Render::FrameExtent extent, const App::UiInput& input)
    {
        extent_ = extent;
        int width, height;
        app.window().getWindowSize(width, height);
        auto camera = app.scene().camera();
        const auto& region = app.viewportRegion();
        const auto logicalSize =
            region.size.x > 0 && region.size.y > 0 ? region.size : Input::PointerValue{double(width), double(height)};
        const bool cameraGesture = input.rotate.down || input.rotate.pressed || input.rotate.released ||
                                   input.pan.down || input.pan.pressed || input.pan.released || input.scroll.y != 0;
        const float threshold = ImGui::GetIO().MouseDragThreshold;
        if (input.select.pressed && !input.captureMouse && !cameraGesture)
            clickCandidate_ = true;
        if (clickCandidate_ &&
            (input.captureMouse || cameraGesture || input.selectDragDistanceSquared > threshold * threshold))
            clickCandidate_ = false;
        if (clickCandidate_ && input.select.released)
        {
            const glm::vec2 position(static_cast<float>(input.position.x / region.size.x),
                                     static_cast<float>(input.position.y / region.size.y));
            const auto hit = app.scene().pick(extent, position);
            operations_.select(hit ? hit->id : Scene::EntityId{});
            clickCandidate_ = false;
        }
        else if (clickCandidate_ && !input.select.down && !input.select.pressed)
            clickCandidate_ = false;
        if (orbit_.apply(camera, input, logicalSize))
            app.scene().setCamera(camera);
        if (input.focusPressed && !input.captureKeyboard)
            focusSelected(app, extent);
    }

    void EditorScene::resetCamera(Editor::EditorApp& app)
    {
        app.scene().setCamera(Scene::Camera({0, 0, 6}));
        orbit_.synchronize(app.scene().camera());
    }

    void EditorScene::ui(Editor::EditorApp& app)
    {
        objects(app);
        assets(app);
        properties(app);
        if (pendingAction_ != PendingAction::None && !pendingSaveAs_)
        {
            bool open = true;
            ImGui::SetNextWindowSize({ImGui::GetFontSize() * 26, 0}, ImGuiCond_Appearing);
            if (ImGui::Begin("Unsaved changes", &open,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking))
            {
                ImGui::TextWrapped("Save changes to %s before continuing?",
                                   operations_.currentFile().empty() ? "(unsaved)"
                                                                     : operations_.currentFile().c_str());
                if (ImGui::Button("Save"))
                {
                    if (!operations_.currentFile().empty())
                    {
                        if (save(app))
                            executePendingAction(app);
                        clearPendingAction();
                    }
                    else
                    {
                        pendingSaveAs_ = true;
                        beginSaveAs();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Discard"))
                {
                    executePendingAction(app);
                    clearPendingAction();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    open = false;
            }
            ImGui::End();
            if (!open)
                clearPendingAction();
        }
        bool confirm = false;
        if (fileAction_ != FileAction::None)
        {
            bool open = true;
            ImGui::SetNextWindowSize({ImGui::GetFontSize() * 30, 0}, ImGuiCond_Appearing);
            ImGui::Begin(fileAction_ == FileAction::Open ? "Open scene" : "Save scene as", &open,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
            ImGui::TextWrapped("Current file: %s",
                               operations_.currentFile().empty() ? "(unsaved)" : operations_.currentFile().c_str());
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
            if (operations_.messageError())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.35f, 1));
                ImGui::TextWrapped("%s", operations_.message().c_str());
                ImGui::PopStyleColor();
            }
            ImGui::End();
            if (!open)
            {
                cancelFileAction();
                clearPendingAction();
                confirm = false;
            }
        }
        if (saveRequested_)
        {
            saveRequested_ = false;
            save(app);
        }
        if (confirm)
        {
            const bool wasSaveAs = fileAction_ == FileAction::SaveAs;
            const bool success = confirmFileAction(app);
            if (wasSaveAs && pendingSaveAs_)
            {
                if (success)
                    executePendingAction(app);
                clearPendingAction();
            }
        }
        handleShortcuts(app);
        drawSelectionOverlay(app);
    }

    void EditorScene::handleShortcuts(Editor::EditorApp& app)
    {
        const auto& raw = app.window().input();
        if (!raw.focused() || raw.focusLost())
        {
            shortcutSuspended_ = true;
            return;
        }
        if (shortcutSuspended_)
        {
            if (ImGui::GetCurrentContext()->InputEventsQueue.empty() && !raw.key(Input::KeyCode::Z).down &&
                !raw.key(Input::KeyCode::Y).down && !ImGui::IsKeyDown(ImGuiKey_Z) && !ImGui::IsKeyDown(ImGuiKey_Y))
                shortcutSuspended_ = false;
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput || fileAction_ != FileAction::None || pendingAction_ != PendingAction::None ||
            ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
            operations_.inTransaction() || orbit_.dragging())
            return;
        if (!io.KeyCtrl || io.KeyAlt || io.KeySuper)
            return;
        if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            undo(app);
        else if ((!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Y, false)) ||
                 (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)))
            redo(app);
    }

    void EditorScene::drawSelectionOverlay(Editor::EditorApp& app) const
    {
        auto* drawList = app.sceneDrawList();
        if (!drawList)
            return;
        const auto& region = app.viewportRegion();
        const auto segments = projectSelectionBox(app.scene(), operations_.selected(), app.sceneExtent(), region);
        if (segments.empty())
            return;
        const ImVec2 clipMin(static_cast<float>(region.origin.x), static_cast<float>(region.origin.y));
        const ImVec2 clipMax(static_cast<float>(region.origin.x + region.size.x),
                             static_cast<float>(region.origin.y + region.size.y));
        const ImU32 color = IM_COL32(255, 200, 64, 255);
        drawList->PushClipRect(clipMin, clipMax, true);
        for (const auto& segment : segments)
            drawList->AddLine({segment.start.x, segment.start.y}, {segment.end.x, segment.end.y}, color, 2.0f);
        drawList->PopClipRect();
    }

    void EditorScene::assets(Editor::EditorApp& app)
    {
        ImGui::SetNextWindowSize({ImGui::GetFontSize() * 22, ImGui::GetFontSize() * 22}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Assets");
        ImGui::TextWrapped("Directory: /%s", assetDirectory_.c_str());
        if (!assetDirectory_.empty())
        {
            if (ImGui::Button("Up"))
                parentDirectory();
            ImGui::SameLine();
        }
        if (ImGui::Button("Refresh"))
            browseDirectory(assetDirectory_);
        ImGui::Text("Listed: %zu | Cached: %zu", assetEntries_.size(), cachedModels());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Listed: directory entries only. Cached: prepared models.\n"
                "Neither count represents CPU loads or live GPU objects.\n"
                "Submitted frames retain GPU resources until completion.");
        if (ImGui::Button("Clear unused models", {-1, 0}))
            collectUnusedModels();
        if (!assetError_.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.35f, 1));
            ImGui::TextWrapped("%s", assetError_.c_str());
            ImGui::PopStyleColor();
        }
        if (operations_.messageError())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.35f, 1));
            ImGui::TextWrapped("%s", operations_.message().c_str());
            ImGui::PopStyleColor();
        }
        std::string enter;
        if (ImGui::BeginTable("##assets", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Name / type", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
            ImGui::TableHeadersRow();
            for (const auto& entry : assetEntries_)
            {
                ImGui::PushID(entry.path.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const auto kind = entry.kind == Resource::AssetKind::Directory    ? "Directory"
                                  : entry.kind == Resource::AssetKind::Model      ? "Model candidate"
                                  : entry.kind == Resource::AssetKind::Texture    ? "PNG"
                                  : entry.kind == Resource::AssetKind::Dependency ? "Dependency"
                                                                                  : "File";
                ImGui::TextUnformatted(entry.name.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s", entry.path.c_str(), kind);
                ImGui::TextDisabled("%s", kind);
                ImGui::TableNextColumn();
                if (entry.kind == Resource::AssetKind::Directory && ImGui::Button("Enter"))
                    enter = entry.path;
                if (entry.kind == Resource::AssetKind::Model && ImGui::Button("Add"))
                    addModel(app, entry.path);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!enter.empty())
            browseDirectory(enter);
        ImGui::End();
    }

    void EditorScene::menu(Editor::EditorApp& app)
    {
        if (!ImGui::BeginMainMenuBar())
            return;
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open..."))
                requestOpen();
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
                requestExit(app);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo()))
                undo(app);
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo()))
                redo(app);
            ImGui::Separator();
            const bool hasSelection =
                operations_.selected().valid() && app.scene().find(operations_.selected()) != nullptr;
            if (ImGui::MenuItem("Duplicate", nullptr, false, hasSelection))
                duplicateSelected(app);
            if (ImGui::MenuItem("Delete", nullptr, false, hasSelection))
                deleteSelected(app);
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

    void EditorScene::statusBar()
    {
        const auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding);
        if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(),
                                        flags))
        {
            if (operations_.messageError())
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.35f, 1));
            const auto text = statusText();
            ImGui::TextUnformatted(text.c_str());
            if (operations_.messageError())
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35);
                ImGui::TextWrapped("%s\n%s", operations_.currentFile().c_str(), operations_.message().c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    void EditorScene::objects(Editor::EditorApp& app)
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
                requestClear(app);
            ImGui::EndPopup();
        }
        ImGui::Separator();
        ImGui::Text("Objects: %zu", app.scene().entities().size());
        if (ImGui::BeginListBox("##entities", {-1, -1}))
        {
            for (const auto& entity : app.scene().entities())
            {
                const auto id = std::to_string(entity.id.value());
                const auto label = (entity.name.empty() ? std::string("未命名") : entity.name) + " (" + id + ")";
                ImGui::PushID(id.c_str());
                if (ImGui::Selectable("##entity", operations_.selected() == entity.id))
                    select(entity.id);
                const auto pos = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddText({pos.x, pos.y}, ImGui::GetColorU32(ImGuiCol_Text), label.data(),
                                                    label.data() + label.size());
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        ImGui::End();
    }

    void EditorScene::properties(Editor::EditorApp& app)
    {
        if (!ImGui::Begin("Properties"))
        {
            ImGui::End();
            if (operations_.inTransaction())
            {
                lightingTransactionActive_ = false;
                backgroundTransactionActive_ = false;
                operations_.commitTransaction(app.scene(), app.clearColor());
            }
            return;
        }
        const auto& raw = app.window().input();
        const bool windowFocused = raw.focused() && !raw.focusLost();
        bool entityWidgetActive = false;
        ImGui::PushItemWidth(-1);
        if (ImGui::CollapsingHeader("Selected object", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (const auto* entity = app.scene().find(operations_.selected()))
            {
                ImGui::Text("ID: %llu", static_cast<unsigned long long>(entity->id.value()));
                ImGui::TextUnformatted("Name");
                const auto nameId = std::to_string(entity->id.value());
                ImGui::PushID(nameId.c_str());
                auto name = pathBuffer(entity->name);
                const auto resize = [](ImGuiInputTextCallbackData* data) {
                    auto* buffer = static_cast<std::vector<char>*>(data->UserData);
                    buffer->resize(static_cast<size_t>(data->BufSize));
                    data->Buf = buffer->data();
                    return 0;
                };
                const bool nameChanged = ImGui::InputText("##entity-name", name.data(), name.size(),
                                                          ImGuiInputTextFlags_CallbackResize, resize, &name);
                if (ImGui::IsItemActivated() && windowFocused)
                {
                    if (lightingTransactionActive_ || backgroundTransactionActive_)
                    {
                        lightingTransactionActive_ = false;
                        backgroundTransactionActive_ = false;
                        operations_.commitTransaction(app.scene(), app.clearColor());
                    }
                    operations_.beginTransaction(app.scene(), app.clearColor());
                }
                if (nameChanged)
                    operations_.rename(app.scene(), app.clearColor(), name.data());
                entityWidgetActive |= ImGui::IsItemActive();
                if (ImGui::IsItemDeactivated())
                    operations_.commitTransaction(app.scene(), app.clearColor());
                ImGui::PopID();
                auto transform = entity->transform;
                auto position = transform.position();
                auto scale = transform.scale();
                auto degrees = glm::degrees(glm::eulerAngles(transform.rotation()));
                bool visible = entity->visible;
                if (ImGui::Checkbox("Visible", &visible))
                    operations_.setVisible(app.scene(), app.clearColor(), visible);
                const auto applyTransformWidget = [&](bool edited) {
                    if ((ImGui::IsItemActivated() || (edited && ImGui::IsItemActive())) && windowFocused)
                    {
                        if (lightingTransactionActive_ || backgroundTransactionActive_)
                        {
                            lightingTransactionActive_ = false;
                            backgroundTransactionActive_ = false;
                            operations_.commitTransaction(app.scene(), app.clearColor());
                        }
                        operations_.beginTransaction(app.scene(), app.clearColor());
                    }
                    if (edited)
                        operations_.setTransform(app.scene(), app.clearColor(), transform);
                    entityWidgetActive |= ImGui::IsItemActive();
                    if (ImGui::IsItemDeactivated())
                        operations_.commitTransaction(app.scene(), app.clearColor());
                };
                ImGui::TextUnformatted("Position");
                bool edited = ImGui::DragFloat3("##position", &position.x, 0.05f);
                if (edited)
                    transform.setPosition(position);
                applyTransformWidget(edited);
                ImGui::TextUnformatted("Rotation");
                edited = ImGui::SliderFloat3("##rotation", &degrees.x, -180, 180, "%.0f", ImGuiSliderFlags_AlwaysClamp);
                if (edited)
                    transform.setRotation(glm::quat(glm::radians(degrees)));
                applyTransformWidget(edited);
                ImGui::TextUnformatted("Scale");
                edited = ImGui::DragFloat3("##scale", &scale.x, 0.05f);
                if (edited)
                    transform.setScale(scale);
                applyTransformWidget(edited);
                if (ImGui::Button("Duplicate"))
                    duplicateSelected(app);
                ImGui::SameLine();
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
            auto clearColor = app.clearColor();
            const bool colorChanged = ImGui::ColorEdit3("##background", clearColor.data(), ImGuiColorEditFlags_NoInputs);
            ImGui::PushID("##background");
            const bool pickerOpen = ImGui::IsPopupOpen("picker");
            ImGui::PopID();
            const bool backgroundActive = (ImGui::IsItemActive() || pickerOpen) && windowFocused;
            if (backgroundActive && !backgroundTransactionActive_)
            {
                if (operations_.inTransaction())
                {
                    lightingTransactionActive_ = false;
                    operations_.commitTransaction(app.scene(), app.clearColor());
                }
                operations_.beginTransaction(app.scene(), app.clearColor());
                backgroundTransactionActive_ = true;
            }
            if (colorChanged)
                operations_.setClearColor(app.scene(), app.clearColor(), clearColor);
            if (!backgroundActive && backgroundTransactionActive_)
            {
                operations_.commitTransaction(app.scene(), app.clearColor());
                backgroundTransactionActive_ = false;
            }
            auto& shadows = app.shadowSettings();
            ImGui::Checkbox("Shadows", &shadows.enabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Fixed light bounds: center (%g, %g, %g), half extents (%g, %g), depth %g..%g.\n"
                    "1024x1024; the range follows light direction, not the camera.",
                    shadows.center.x, shadows.center.y, shadows.center.z, shadows.halfExtent.x, shadows.halfExtent.y,
                    shadows.nearPlane, shadows.farPlane);
            ImGui::TextUnformatted("Receiver depth bias");
            ImGui::SliderFloat("##shadowBias", &shadows.depthBias, 0, 0.01f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::TextWrapped("Shadow preview settings are not saved in scene files.");
            ImGui::Separator();
            auto lighting = app.scene().lighting();
            const auto lightingState = App::lightingWidget(lighting);
            const bool lightingActive = lightingState.active && windowFocused;
            if (lightingActive && !lightingTransactionActive_)
            {
                if (operations_.inTransaction())
                {
                    backgroundTransactionActive_ = false;
                    operations_.commitTransaction(app.scene(), app.clearColor());
                }
                operations_.beginTransaction(app.scene(), app.clearColor());
                lightingTransactionActive_ = true;
            }
            if (lightingState.changed)
                operations_.setLighting(app.scene(), app.clearColor(), lighting);
            if (!lightingActive && lightingTransactionActive_)
            {
                operations_.commitTransaction(app.scene(), app.clearColor());
                lightingTransactionActive_ = false;
            }
        }
        else if (lightingTransactionActive_ || backgroundTransactionActive_)
        {
            lightingTransactionActive_ = false;
            backgroundTransactionActive_ = false;
            operations_.commitTransaction(app.scene(), app.clearColor());
        }
        if (operations_.inTransaction() && !entityWidgetActive && !lightingTransactionActive_ &&
            !backgroundTransactionActive_)
            operations_.commitTransaction(app.scene(), app.clearColor());
        ImGui::PopItemWidth();
        ImGui::End();
    }

    std::vector<char> EditorScene::pathBuffer(std::string_view path)
    {
        std::vector<char> buffer(std::max(size_t{1024}, path.size() + 1), '\0');
        std::copy(path.begin(), path.end(), buffer.begin());
        return buffer;
    }

    std::string_view EditorScene::creationPath(bool useObj)
    {
        return useObj ? "models/utah_teapot/utah_teapot.obj" : "models/avocado/glTF/Avocado.gltf";
    }

    Scene::Transform EditorScene::initialTransform(bool useObj)
    {
        return Scene::Transform({0, useObj ? 0.0f : -0.78f, 0}, {1, 0, 0, 0}, glm::vec3(useObj ? 0.075f : 24.0f));
    }

    Editor::Callbacks makeCallbacks(std::string startupScene)
    {
        auto demo = std::make_shared<EditorScene>();
        return {[demo, startupScene = std::move(startupScene)](auto& app) {
                    demo->initialize(app, startupScene);
                    app.shadowSettings().enabled = true;
                },
                {},
                [demo](auto& app) { demo->ui(app); },
                [demo](auto& app) { demo->cancelInteraction(app); },
                [demo](auto& app, auto extent, const auto& input) { demo->input(app, extent, input); },
                [demo](auto& app) { demo->layout(app); },
                [demo](auto& app) { demo->requestClose(app); }};
    }
}  // namespace Incubator::Editor
