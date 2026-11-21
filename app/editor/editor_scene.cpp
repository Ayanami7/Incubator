#include "editor_scene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/common/lighting_controls.h"
#include "app/common/viewport.h"
#include "editor_theme.h"
#include "runtime/model/prepared_model.h"

namespace Incubator::Editor
{
    bool EditorScene::addModel(Editor::EditorApp& app, std::string_view path, const Scene::Transform& transform)
    {
        return controller_.addModel(app, path, transform);
    }

    void EditorScene::initialize(Editor::EditorApp& app, std::string_view path)
    {
        controller_.initialize(app, path);
    }

    Scene::EntityId EditorScene::createEntity(Editor::EditorApp& app)
    {
        return controller_.createEntity(app);
    }

    bool EditorScene::browseDirectory(std::string_view path)
    {
        return controller_.browseDirectory(path);
    }

    const std::string& EditorScene::assetDirectory() const
    {
        return controller_.assetDirectory();
    }

    const std::vector<Resource::AssetEntry>& EditorScene::assetEntries() const
    {
        return controller_.assetEntries();
    }

    const std::string& EditorScene::assetError() const
    {
        return controller_.assetError();
    }

    bool EditorScene::parentDirectory()
    {
        return controller_.parentDirectory();
    }

    void EditorScene::selectModel(bool useObj)
    {
        controller_.selectModel(useObj);
    }

    bool EditorScene::save(Editor::EditorApp& app, std::string_view path)
    {
        return controller_.save(app, path);
    }

    bool EditorScene::load(Editor::EditorApp& app, std::string_view path, bool rethrow)
    {
        return controller_.load(app, path, rethrow);
    }

    bool EditorScene::save(Editor::EditorApp& app)
    {
        return controller_.save(app);
    }

    void EditorScene::beginSaveAs()
    {
        controller_.beginSaveAs();
    }

    void EditorScene::beginOpen()
    {
        controller_.beginOpen();
    }

    void EditorScene::cancelFileAction()
    {
        controller_.cancelFileAction();
    }

    void EditorScene::requestOpen(Editor::EditorApp& app)
    {
        controller_.requestOpen(app);
    }

    void EditorScene::requestClear(Editor::EditorApp& app)
    {
        controller_.requestClear(app);
    }

    void EditorScene::requestExit(Editor::EditorApp& app)
    {
        controller_.requestExit(app);
    }

    void EditorScene::requestClose(Editor::EditorApp& app)
    {
        controller_.requestClose(app);
    }

    void EditorScene::operationPath(std::string_view path)
    {
        controller_.operationPath(path);
    }

    bool EditorScene::confirmFileAction(Editor::EditorApp& app)
    {
        return controller_.confirmFileAction(app);
    }

    void EditorScene::layout(Editor::EditorApp& app)
    {
        menu(app);
        toolbar(app);
        statusBar(app);
        workspace_.begin();
        app.sceneViewport("Scene", &viewportOpen_);
        controller_.syncViewportExtent(app.sceneExtent());
    }

    void EditorScene::restoreLayout()
    {
        workspace_.reset();
    }

    std::string EditorScene::statusText() const
    {
        return controller_.statusText();
    }

    const std::string& EditorScene::message() const
    {
        return controller_.message();
    }

    std::string_view EditorScene::scenePath() const
    {
        return controller_.scenePath();
    }

    Scene::EntityId EditorScene::selected() const
    {
        return controller_.selected();
    }

    void EditorScene::select(Scene::EntityId id)
    {
        controller_.select(id);
    }

    void EditorScene::select(Editor::EditorApp& app, Scene::EntityId id)
    {
        controller_.select(app, id);
    }

    GizmoMode EditorScene::gizmoMode() const noexcept
    {
        return controller_.gizmoMode();
    }

    void EditorScene::setGizmoMode(GizmoMode mode) noexcept
    {
        controller_.setGizmoMode(mode);
    }

    GizmoAxis EditorScene::hoveredGizmoAxis() const noexcept
    {
        return controller_.hoveredGizmoAxis();
    }

    bool EditorScene::gizmoDragging() const noexcept
    {
        return controller_.gizmoDragging();
    }

    GizmoAxis EditorScene::activeGizmoAxis() const noexcept
    {
        return controller_.activeGizmoAxis();
    }

    void EditorScene::deleteSelected(Editor::EditorApp& app)
    {
        controller_.deleteSelected(app);
    }

    bool EditorScene::undo(Editor::EditorApp& app)
    {
        return controller_.undo(app);
    }

    bool EditorScene::redo(Editor::EditorApp& app)
    {
        return controller_.redo(app);
    }

    bool EditorScene::setEntityShading(Editor::EditorApp& app, Render::ShadingMode shading)
    {
        return controller_.setEntityShading(app, shading);
    }

    Scene::EntityId EditorScene::duplicateSelected(Editor::EditorApp& app)
    {
        return controller_.duplicateSelected(app);
    }

    void EditorScene::clear(Editor::EditorApp& app)
    {
        controller_.clear(app);
    }

    void EditorScene::cancelInteraction(Editor::EditorApp& app)
    {
        controller_.cancelInteraction(app);
    }

    bool EditorScene::focusSelected(Editor::EditorApp& app, Render::FrameExtent extent)
    {
        return controller_.focusSelected(app, extent);
    }

    void EditorScene::input(Editor::EditorApp& app, Render::FrameExtent extent, const App::UiInput& input)
    {
        controller_.input(app, extent, input, ImGui::GetIO().MouseDragThreshold);
    }

    void EditorScene::resetCamera(Editor::EditorApp& app)
    {
        controller_.resetCamera(app);
    }

    void EditorScene::ui(Editor::EditorApp& app)
    {
        objects(app);
        assets(app);
        properties(app);
        if (controller_.pendingAction() != PendingAction::None && !controller_.pendingSaveAs())
        {
            bool open = true;
            ImGui::SetNextWindowSize({ImGui::GetFontSize() * 26, 0}, ImGuiCond_Appearing);
            if (ImGui::Begin("Unsaved changes", &open,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking))
            {
                const auto file = controller_.scenePath();
                ImGui::TextUnformatted("Save changes before continuing?");
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::accentColor());
                ImGui::TextWrapped("%s", file.empty() ? "(unsaved)" : std::string(file).c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (ImGui::Button("Save"))
                    controller_.resolveUnsavedSave(app);
                ImGui::SameLine();
                if (ImGui::Button("Discard"))
                    controller_.discardPendingAction(app);
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    open = false;
            }
            ImGui::End();
            if (!open)
                controller_.cancelPendingAction();
        }
        bool confirm = false;
        const FileAction currentFileAction = controller_.fileAction();
        if (currentFileAction != FileAction::None && currentFileAction != lastFileAction_)
            controller_.refreshSceneFiles(app.fileIO());
        lastFileAction_ = currentFileAction;
        if (currentFileAction != FileAction::None)
        {
            bool open = true;
            ImGui::SetNextWindowSize({ImGui::GetFontSize() * 30, 0}, ImGuiCond_Appearing);
            ImGui::Begin(currentFileAction == FileAction::Open ? "Open scene" : "Save scene as", &open,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
            const auto file = controller_.scenePath();
            ImGui::TextWrapped("Current file: %s", file.empty() ? "(unsaved)" : std::string(file).c_str());
            ImGui::Spacing();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Scene files");
            ImGui::SameLine();
            if (ImGui::SmallButton("Refresh##scene-files"))
                controller_.refreshSceneFiles(app.fileIO());
            if (ImGui::BeginListBox("##scene-files", {-1, ImGui::GetFontSize() * 7.0f}))
            {
                const auto& files = controller_.sceneFiles();
                if (files.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::mutedTextColor());
                    ImGui::TextUnformatted("No .json scenes found.");
                    ImGui::PopStyleColor();
                }
                else
                {
                    const std::string_view currentInput(controller_.pathBufferData().data());
                    for (const auto& entry : files)
                    {
                        const bool isSelected = currentInput == entry;
                        if (ImGui::Selectable(entry.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick))
                        {
                            controller_.operationPath(entry);
                            if (currentFileAction == FileAction::Open &&
                                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                confirm = true;
                        }
                    }
                }
                ImGui::EndListBox();
            }
            ImGui::TextUnformatted("Target path");
            ImGui::SetNextItemWidth(-1);
            auto& pathBuf = controller_.pathBufferData();
            ImGui::InputText("##path", pathBuf.data(), pathBuf.size());
            if (ImGui::Button(currentFileAction == FileAction::Open ? "Open" : "Save"))
                confirm = true;
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                open = false;
            ImGui::TextWrapped(
                "Saving a deployed copy does not update source assets. Build cleanup deletes it; "
                "asset redeployment may overwrite it.");
            if (controller_.messageError())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::errorColor());
                ImGui::TextWrapped("%s", controller_.message().c_str());
                ImGui::PopStyleColor();
            }
            ImGui::End();
            if (!open)
            {
                controller_.cancelFileAction();
                controller_.cancelPendingAction();
                lastFileAction_ = FileAction::None;
                confirm = false;
            }
        }
        if (controller_.consumeSaveRequested())
            save(app);
        if (confirm)
            confirmFileAction(app);
        handleShortcuts(app);
        drawSelectionOverlay(app);
    }

    void EditorScene::handleShortcuts(Editor::EditorApp& app)
    {
        const auto& raw = app.window().input();
        const ImGuiIO& io = ImGui::GetIO();
        ShortcutInput state;
        state.focused = raw.focused();
        state.focusLost = raw.focusLost();
        state.inputQueueEmpty = ImGui::GetCurrentContext()->InputEventsQueue.empty();
        state.wantTextInput = io.WantTextInput;
        state.popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        state.keyAlt = io.KeyAlt;
        state.keySuper = io.KeySuper;
        state.keyCtrl = io.KeyCtrl;
        state.keyShift = io.KeyShift;
        state.mouseRightDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        state.mouseMiddleDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        state.wDown = raw.key(Input::KeyCode::W).down || ImGui::IsKeyDown(ImGuiKey_W);
        state.eDown = raw.key(Input::KeyCode::E).down || ImGui::IsKeyDown(ImGuiKey_E);
        state.rDown = raw.key(Input::KeyCode::R).down || ImGui::IsKeyDown(ImGuiKey_R);
        state.zDown = raw.key(Input::KeyCode::Z).down || ImGui::IsKeyDown(ImGuiKey_Z);
        state.yDown = raw.key(Input::KeyCode::Y).down || ImGui::IsKeyDown(ImGuiKey_Y);
        state.wPressed = ImGui::IsKeyPressed(ImGuiKey_W, false);
        state.ePressed = ImGui::IsKeyPressed(ImGuiKey_E, false);
        state.rPressed = ImGui::IsKeyPressed(ImGuiKey_R, false);
        state.zPressed = ImGui::IsKeyPressed(ImGuiKey_Z, false);
        state.yPressed = ImGui::IsKeyPressed(ImGuiKey_Y, false);
        controller_.handleShortcuts(app, state);
    }

    void EditorScene::drawSelectionOverlay(Editor::EditorApp& app) const
    {
        auto* drawList = app.sceneDrawList();
        if (!drawList)
            return;
        const auto overlay = controller_.buildViewportOverlay(app);
        if (overlay.lines.empty())
            return;
        drawList->PushClipRect({overlay.clipMin.x, overlay.clipMin.y}, {overlay.clipMax.x, overlay.clipMax.y}, true);
        for (const auto& line : overlay.lines)
        {
            const ImU32 color = IM_COL32(line.color.r, line.color.g, line.color.b, line.color.a);
            drawList->AddLine({line.segment.start.x, line.segment.start.y}, {line.segment.end.x, line.segment.end.y},
                              color, line.thickness);
        }
        drawList->PopClipRect();
    }

    void EditorScene::assets(Editor::EditorApp& app)
    {
        ImGui::SetNextWindowSize({ImGui::GetFontSize() * 22, ImGui::GetFontSize() * 22}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Assets");
        // 可点击路径面包屑导航栏
        std::string breadcrumbTarget;
        bool hasBreadcrumbTarget = false;
        ImGui::PushID("breadcrumb");
        if (ImGui::SmallButton("/"))
        {
            breadcrumbTarget.clear();
            hasBreadcrumbTarget = true;
        }
        if (!controller_.assetDirectory().empty())
        {
            const std::string& dir = controller_.assetDirectory();
            size_t start = 0;
            int segIndex = 0;
            while (start < dir.size())
            {
                const size_t slash = dir.find('/', start);
                const size_t end = slash == std::string::npos ? dir.size() : slash;
                const std::string segment = dir.substr(start, end - start);
                const std::string cumulative = dir.substr(0, end);
                if (!segment.empty())
                {
                    ImGui::SameLine(0.0f, 4.0f);
                    ImGui::TextDisabled(">");
                    ImGui::SameLine(0.0f, 4.0f);
                    ImGui::PushID(segIndex++);
                    if (ImGui::SmallButton(segment.c_str()))
                    {
                        breadcrumbTarget = cumulative;
                        hasBreadcrumbTarget = true;
                    }
                    ImGui::PopID();
                }
                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }
        }
        ImGui::PopID();
        if (hasBreadcrumbTarget)
            browseDirectory(breadcrumbTarget);

        if (!controller_.assetDirectory().empty())
        {
            if (ImGui::Button("Up"))
                parentDirectory();
            ImGui::SameLine();
        }
        if (ImGui::Button("Refresh"))
            browseDirectory(controller_.assetDirectory());
        ImGui::Text("Listed: %zu | Cached: %zu", controller_.assetEntries().size(), cachedModels());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Listed: directory entries only. Cached: prepared models.\n"
                "Neither count represents CPU loads or live GPU objects.\n"
                "Submitted frames retain GPU resources until completion.");
        if (ImGui::Button("Clear unused models", {-1, 0}))
            collectUnusedModels();
        if (!controller_.assetError().empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::errorColor());
            ImGui::TextWrapped("%s", controller_.assetError().c_str());
            ImGui::PopStyleColor();
        }
        if (controller_.messageError())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::errorColor());
            ImGui::TextWrapped("%s", controller_.message().c_str());
            ImGui::PopStyleColor();
        }
        std::string enter;
        const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##assets", 2, tableFlags))
        {
            ImGui::TableSetupColumn("Name / type", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.8f);
            ImGui::TableHeadersRow();
            for (const auto& entry : controller_.assetEntries())
            {
                ImGui::PushID(entry.path.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const char* kind = "File";
                const char* badge = "[FILE]";
                ImVec4 badgeColor = EditorTheme::mutedTextColor();
                if (entry.kind == Resource::AssetKind::Directory)
                {
                    kind = "Directory";
                    badge = "[DIR]";
                    badgeColor = EditorTheme::badgeDirColor();
                }
                else if (entry.kind == Resource::AssetKind::Model)
                {
                    kind = "Model candidate";
                    badge = "[MODEL]";
                    badgeColor = EditorTheme::badgeModelColor();
                }
                else if (entry.kind == Resource::AssetKind::Texture)
                {
                    kind = "PNG";
                    badge = "[PNG]";
                    badgeColor = EditorTheme::badgeTextureColor();
                }
                else if (entry.kind == Resource::AssetKind::Dependency)
                {
                    kind = "Dependency";
                    badge = "[DEP]";
                }
                ImGui::PushStyleColor(ImGuiCol_Text, badgeColor);
                ImGui::TextUnformatted(badge);
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::TextUnformatted(entry.name.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s", entry.path.c_str(), kind);
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
                requestOpen(app);
            if (ImGui::MenuItem("Save"))
                controller_.requestSave();
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
            const bool hasSelection = selected().valid() && app.scene().find(selected()) != nullptr;
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

    void EditorScene::toolbar(Editor::EditorApp& app)
    {
        const auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
        const auto& style = ImGui::GetStyle();
        const float barHeight = ImGui::GetFrameHeight() + style.FramePadding.y * 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.FramePadding);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::rgba(0x19, 0x1C, 0x20));
        if (ImGui::BeginViewportSideBar("##toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, barHeight, flags))
        {
            const auto verticalSeparator = []() {
                ImGui::SameLine();
                ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                ImGui::SameLine();
            };

            // 文件操作组（动态保存标签使用固定 ID 防止脏状态切换时吞掉点击）
            if (ImGui::Button("Open..."))
                requestOpen(app);
            ImGui::SameLine();
            if (ImGui::Button(isDirty() ? "Save *###toolbar-save" : "Save###toolbar-save"))
                controller_.requestSave();
            ImGui::SameLine();
            if (ImGui::Button("Save As..."))
                beginSaveAs();

            verticalSeparator();

            // 历史操作组
            ImGui::BeginDisabled(!canUndo());
            if (ImGui::Button("Undo"))
                undo(app);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Undo (Ctrl+Z)");
            ImGui::SameLine();
            ImGui::BeginDisabled(!canRedo());
            if (ImGui::Button("Redo"))
                redo(app);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Redo (Ctrl+Y)");

            verticalSeparator();

            // Gizmo 模式切换组
            const auto gizmoToolbarButton = [&](const char* label, GizmoMode mode) {
                const bool active = gizmoMode() == mode;
                if (active)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::accentColor());
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::accentColor());
                }
                if (ImGui::Button(label))
                    setGizmoMode(mode);
                if (active)
                    ImGui::PopStyleColor(2);
            };
            gizmoToolbarButton("Translate (W)", GizmoMode::Translate);
            ImGui::SameLine();
            gizmoToolbarButton("Rotate (E)", GizmoMode::Rotate);
            ImGui::SameLine();
            gizmoToolbarButton("Scale (R)", GizmoMode::Scale);

            verticalSeparator();

            // 视图与阴影快捷组
            const auto* selectedEntity = app.scene().find(selected());
            const bool canFocus = selectedEntity && selectedEntity->model && selectedEntity->model->bounds();
            ImGui::BeginDisabled(!canFocus);
            if (ImGui::Button("Focus (F)"))
            {
                const auto focusExtent = (controller_.lastExtent().width > 0 && controller_.lastExtent().height > 0)
                                             ? controller_.lastExtent()
                                             : app.sceneExtent();
                focusSelected(app, focusExtent);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Reset View"))
                resetCamera(app);
            ImGui::SameLine();
            ImGui::Checkbox("Shadows", &app.shadowSettings().enabled);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void EditorScene::statusBar(Editor::EditorApp& app)
    {
        const auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
        const auto& style = ImGui::GetStyle();
        const float barHeight = ImGui::GetFrameHeight() + style.FramePadding.y * 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.FramePadding);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::rgba(0x19, 0x1C, 0x20));
        if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down, barHeight, flags))
        {
            if (controller_.messageError())
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::errorColor());
            const auto text = statusText();
            ImGui::TextUnformatted(text.c_str());
            if (controller_.messageError())
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35);
                ImGui::TextWrapped("%s\n%s", std::string(controller_.scenePath()).c_str(),
                                   controller_.message().c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            char statsBuffer[96]{};
            std::snprintf(statsBuffer, sizeof(statsBuffer), "Entities: %zu | Cached: %zu",
                          app.scene().entities().size(), cachedModels());
            const float statsWidth = ImGui::CalcTextSize(statsBuffer).x;
            const float itemRightLocal = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
            const float rightEdge = ImGui::GetWindowContentRegionMax().x;
            if (rightEdge - itemRightLocal > statsWidth + ImGui::GetStyle().ItemSpacing.x * 2.0f)
            {
                ImGui::SameLine(rightEdge - statsWidth);
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::mutedTextColor());
                ImGui::TextUnformatted(statsBuffer);
                ImGui::PopStyleColor();
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void EditorScene::objects(Editor::EditorApp& app)
    {
        ImGui::Begin("Objects");
        ImGui::TextUnformatted("New model");
        ImGui::SetNextItemWidth(-1);
        int modelIndex = controller_.useObjModel() ? 1 : 0;
        if (ImGui::Combo("##model", &modelIndex, "Avocado\0Teapot\0"))
            controller_.selectModel(modelIndex == 1);
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
            auto* drawList = ImGui::GetWindowDrawList();
            for (const auto& entity : app.scene().entities())
            {
                const auto id = std::to_string(entity.id.value());
                const std::string displayName = entity.name.empty() ? std::string("未命名") : entity.name;
                const std::string idSuffix = " (#" + id + ")";
                ImGui::PushID(id.c_str());
                bool visible = entity.visible;
                if (ImGui::Checkbox("##vis", &visible))
                    controller_.setVisible(app, entity.id, visible);
                ImGui::SameLine();
                if (ImGui::Selectable("##entity", controller_.selected() == entity.id))
                    select(app, entity.id);
                const auto pos = ImGui::GetItemRectMin();
                const float textY = pos.y + (ImGui::GetItemRectSize().y - ImGui::GetFontSize()) * 0.5f;
                const ImU32 nameColor = entity.visible
                                            ? ImGui::GetColorU32(ImGuiCol_Text)
                                            : ImGui::GetColorU32(EditorTheme::mutedTextColor());
                drawList->AddText({pos.x + 2.0f, textY}, nameColor, displayName.data(),
                                  displayName.data() + displayName.size());
                const float nameWidth = ImGui::CalcTextSize(displayName.c_str()).x;
                drawList->AddText({pos.x + 2.0f + nameWidth, textY},
                                  ImGui::GetColorU32(EditorTheme::mutedTextColor()), idSuffix.data(),
                                  idSuffix.data() + idSuffix.size());
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        ImGui::End();
    }

    void EditorScene::properties(Editor::EditorApp& app)
    {
        controller_.syncPanelTransactionState();
        if (!ImGui::Begin("Properties"))
        {
            ImGui::End();
            controller_.commitPanelTransactions(app);
            return;
        }
        const auto& raw = app.window().input();
        const bool windowFocused = raw.focused() && !raw.focusLost();
        bool entityWidgetActive = false;
        auto& operations = controller_.operations();
        const auto beginEntityTransaction = [&]() {
            controller_.beginPanelTransaction(app, PanelTransactionKind::Entity);
        };
        const auto finishEntityTransaction = [&]() {
            if (controller_.activePanelTransaction() == PanelTransactionKind::Entity)
                controller_.commitPanelTransactions(app);
        };
        const auto& style = ImGui::GetStyle();
        const float availWidth = ImGui::GetContentRegionAvail().x;
        const float fontSize = ImGui::GetFontSize();
        const float labelWidth = std::clamp(availWidth * 0.34f, fontSize * 5.5f, fontSize * 8.0f);
        const float controlWidth = std::max(60.0f, availWidth - labelWidth);
        const float resetBtnWidth = ImGui::CalcTextSize("R").x + style.FramePadding.x * 2.0f;
        const float transformWidth = std::max(60.0f, controlWidth - resetBtnWidth - style.ItemSpacing.x);
        const float halfWidth = std::max(60.0f, (availWidth - style.ItemSpacing.x) * 0.5f);
        const auto rowLabel = [&](const char* label) {
            const float rowStartX = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(rowStartX + labelWidth);
        };
        const auto rowAxisLabel = [&](const char* label) {
            const float rowStartX = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const float barX = itemMax.x + 5.0f;
            const float barH = std::max(2.0f, (itemMax.y - itemMin.y - 4.0f) / 3.0f);
            auto* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled({barX, itemMin.y + 1.0f}, {barX + 3.0f, itemMin.y + 1.0f + barH},
                                    ImGui::GetColorU32(EditorTheme::axisXColor()));
            drawList->AddRectFilled({barX, itemMin.y + 2.0f + barH}, {barX + 3.0f, itemMin.y + 2.0f + barH * 2.0f},
                                    ImGui::GetColorU32(EditorTheme::axisYColor()));
            drawList->AddRectFilled({barX, itemMin.y + 3.0f + barH * 2.0f},
                                    {barX + 3.0f, itemMin.y + 3.0f + barH * 3.0f},
                                    ImGui::GetColorU32(EditorTheme::axisZColor()));
            ImGui::SameLine(rowStartX + labelWidth);
        };

        if (ImGui::CollapsingHeader("Selected object", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (const auto* entity = app.scene().find(controller_.selected()))
            {
                rowLabel("ID");
                ImGui::Text("#%llu", static_cast<unsigned long long>(entity->id.value()));
                rowLabel("Name");
                const auto nameId = std::to_string(entity->id.value());
                ImGui::PushID(nameId.c_str());
                auto name = EditorController::pathBuffer(entity->name);
                const auto resize = [](ImGuiInputTextCallbackData* data) {
                    auto* buffer = static_cast<std::vector<char>*>(data->UserData);
                    buffer->resize(static_cast<size_t>(data->BufSize));
                    data->Buf = buffer->data();
                    return 0;
                };
                ImGui::SetNextItemWidth(controlWidth);
                const bool nameChanged = ImGui::InputText("##entity-name", name.data(), name.size(),
                                                          ImGuiInputTextFlags_CallbackResize, resize, &name);
                if (ImGui::IsItemActivated() && windowFocused)
                    beginEntityTransaction();
                if (nameChanged)
                    operations.rename(app.scene(), app.clearColor(), name.data());
                entityWidgetActive |= ImGui::IsItemActive();
                if (ImGui::IsItemDeactivated())
                    finishEntityTransaction();
                ImGui::PopID();
                auto transform = entity->transform;
                auto position = transform.position();
                auto scale = transform.scale();
                auto degrees = glm::degrees(glm::eulerAngles(transform.rotation()));
                bool visible = entity->visible;
                rowLabel("Visible");
                if (ImGui::Checkbox("##visible", &visible))
                    operations.setVisible(app.scene(), app.clearColor(), visible);
                if (entity->model)
                {
                    int shadingIndex = 1;
                    switch (entity->model->source().options().shading)
                    {
                        case Render::ShadingMode::Unlit:
                            shadingIndex = 0;
                            break;
                        case Render::ShadingMode::Lambert:
                            shadingIndex = 1;
                            break;
                        case Render::ShadingMode::Pbr:
                            shadingIndex = 2;
                            break;
                    }
                    rowLabel("Shading");
                    ImGui::SetNextItemWidth(controlWidth);
                    if (ImGui::Combo("##entity-shading", &shadingIndex, "Unlit\0Lambert\0PBR\0"))
                    {
                        const Render::ShadingMode targetShading =
                            shadingIndex == 0 ? Render::ShadingMode::Unlit
                                              : (shadingIndex == 2 ? Render::ShadingMode::Pbr
                                                                   : Render::ShadingMode::Lambert);
                        controller_.setEntityShading(app, targetShading);
                    }
                }
                const auto applyTransformWidget = [&](bool edited) {
                    const bool activateStart = ImGui::IsItemActivated();
                    const bool entityActive = controller_.activePanelTransaction() == PanelTransactionKind::Entity;
                    if ((activateStart || (edited && ImGui::IsItemActive() && !entityActive)) && windowFocused)
                        beginEntityTransaction();
                    if (edited)
                        operations.setTransform(app.scene(), app.clearColor(), transform);
                    entityWidgetActive |= ImGui::IsItemActive();
                    if (ImGui::IsItemDeactivated())
                        finishEntityTransaction();
                };
                rowAxisLabel("Position");
                ImGui::SetNextItemWidth(transformWidth);
                bool edited = ImGui::DragFloat3("##position", &position.x, 0.05f);
                if (edited)
                    transform.setPosition(position);
                applyTransformWidget(edited);
                ImGui::SameLine();
                if (ImGui::Button("R##reset-pos"))
                {
                    auto resetTransform = entity->transform;
                    resetTransform.setPosition(glm::vec3(0.0f));
                    operations.setTransform(app.scene(), app.clearColor(), resetTransform);
                }
                rowAxisLabel("Rotation");
                ImGui::SetNextItemWidth(transformWidth);
                edited = ImGui::SliderFloat3("##rotation", &degrees.x, -180, 180, "%.0f", ImGuiSliderFlags_AlwaysClamp);
                if (edited)
                    transform.setRotation(glm::quat(glm::radians(degrees)));
                applyTransformWidget(edited);
                ImGui::SameLine();
                if (ImGui::Button("R##reset-rot"))
                {
                    auto resetTransform = entity->transform;
                    resetTransform.setRotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                    operations.setTransform(app.scene(), app.clearColor(), resetTransform);
                }
                rowAxisLabel("Scale");
                ImGui::SetNextItemWidth(transformWidth);
                edited = ImGui::DragFloat3("##scale", &scale.x, 0.05f);
                if (edited)
                    transform.setScale(scale);
                applyTransformWidget(edited);
                ImGui::SameLine();
                if (ImGui::Button("R##reset-scale"))
                {
                    auto resetTransform = entity->transform;
                    resetTransform.setScale(glm::vec3(1.0f));
                    operations.setTransform(app.scene(), app.clearColor(), resetTransform);
                }
                if (ImGui::Button("Duplicate", {halfWidth, 0.0f}))
                    duplicateSelected(app);
                ImGui::SameLine();
                if (ImGui::Button("Delete selected", {halfWidth, 0.0f}))
                    deleteSelected(app);
            }
            else
                ImGui::TextWrapped("Select an object to edit.");
            ImGui::Separator();
            const float thirdWidth = std::max(40.0f, (availWidth - style.ItemSpacing.x * 2.0f) / 3.0f);
            const auto gizmoModeButton = [&](const char* label, GizmoMode mode) {
                const bool active = gizmoMode() == mode;
                if (active)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::accentColor());
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::accentColor());
                }
                if (ImGui::Button(label, {thirdWidth, 0.0f}))
                    setGizmoMode(mode);
                if (active)
                    ImGui::PopStyleColor(2);
            };
            gizmoModeButton("Translate (W)", GizmoMode::Translate);
            ImGui::SameLine();
            gizmoModeButton("Rotate (E)", GizmoMode::Rotate);
            ImGui::SameLine();
            gizmoModeButton("Scale (R)", GizmoMode::Scale);
        }
        if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::Button("Focus selected", {halfWidth, 0.0f}))
            {
                const auto focusExtent = (controller_.lastExtent().width > 0 && controller_.lastExtent().height > 0)
                                             ? controller_.lastExtent()
                                             : app.sceneExtent();
                focusSelected(app, focusExtent);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset view", {halfWidth, 0.0f}))
                resetCamera(app);
        }
        if (ImGui::CollapsingHeader("Scene settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            rowLabel("Background");
            auto clearColor = app.clearColor();
            const bool colorChanged = ImGui::ColorEdit3("##background", clearColor.data(), ImGuiColorEditFlags_NoInputs);
            ImGui::PushID("##background");
            const bool pickerOpen = ImGui::IsPopupOpen("picker");
            ImGui::PopID();
            const bool backgroundActive = (ImGui::IsItemActive() || pickerOpen) && windowFocused;
            if (backgroundActive && controller_.activePanelTransaction() != PanelTransactionKind::Background)
                controller_.beginPanelTransaction(app, PanelTransactionKind::Background);
            if (colorChanged)
                operations.setClearColor(app.scene(), app.clearColor(), clearColor);
            if (!backgroundActive && controller_.activePanelTransaction() == PanelTransactionKind::Background)
                controller_.commitPanelTransactions(app);
            auto& shadows = app.shadowSettings();
            rowLabel("Shadows");
            ImGui::Checkbox("##shadows", &shadows.enabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Fixed light bounds: center (%g, %g, %g), half extents (%g, %g), depth %g..%g.\n"
                    "1024x1024; the range follows light direction, not the camera.",
                    shadows.center.x, shadows.center.y, shadows.center.z, shadows.halfExtent.x, shadows.halfExtent.y,
                    shadows.nearPlane, shadows.farPlane);
            rowLabel("Depth bias");
            ImGui::SetNextItemWidth(controlWidth);
            ImGui::SliderFloat("##shadowBias", &shadows.depthBias, 0, 0.01f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::TextDisabled("Shadow preview settings are not saved in scene files.");
            ImGui::Separator();
            auto lighting = app.scene().lighting();
            const auto lightingState = App::lightingWidget(lighting);
            const bool lightingActive = lightingState.active && windowFocused;
            if (lightingActive && controller_.activePanelTransaction() != PanelTransactionKind::Lighting)
                controller_.beginPanelTransaction(app, PanelTransactionKind::Lighting);
            if (lightingState.changed)
                operations.setLighting(app.scene(), app.clearColor(), lighting);
            if (!lightingActive && controller_.activePanelTransaction() == PanelTransactionKind::Lighting)
                controller_.commitPanelTransactions(app);
        }
        else if (controller_.activePanelTransaction() == PanelTransactionKind::Lighting ||
                 controller_.activePanelTransaction() == PanelTransactionKind::Background)
        {
            controller_.commitPanelTransactions(app);
        }
        if (controller_.activePanelTransaction() == PanelTransactionKind::Entity && !entityWidgetActive)
            finishEntityTransaction();
        ImGui::End();
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
