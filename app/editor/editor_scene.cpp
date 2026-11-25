#include "editor_scene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/matrix.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/common/lighting_controls.h"
#include "app/common/viewport.h"
#include "editor_theme.h"
#include "runtime/core/error/error.h"
#include "runtime/model/prepared_model.h"
#include "viewport_screenshot.h"

namespace Incubator::Editor
{
    EditorScene::EditorScene(ExternalModelPicker picker, ScreenshotPathPicker screenshotPicker)
        : pickExternalModel_(std::move(picker)), pickScreenshotPath_(std::move(screenshotPicker))
    {}

    bool EditorScene::addModel(Editor::EditorApp& app, std::string_view path, const Scene::Transform& transform)
    {
        return controller_.addModel(app, path, transform);
    }

    void EditorScene::initialize(Editor::EditorApp& app, std::string_view path)
    {
        controller_.initialize(app, path);
        app.setDebugLineBuilder([this, &app](std::vector<Render::DebugLine>& out) {
            out = buildViewportDebugLines(app.scene(), controller_.selected(), debugOptions_);
        });
    }

    bool EditorScene::exportScreenshot(Editor::EditorApp& app, const std::filesystem::path& path)
    {
        try
        {
            const auto captured = app.renderer().captureLastSceneViewport();
            if (!captured)
            {
                controller_.operations().setStatus("No rendered viewport image available to export", true);
                return false;
            }
            writeRgba8Png(app.fileIO(), path, captured->extent.width, captured->extent.height, captured->rgba8);
            controller_.operations().setStatus(fmt::format("Exported viewport PNG to '{}'", path.filename().string()));
            return true;
        }
        catch (const Error::Exception& e)
        {
            controller_.operations().setStatus(fmt::format("Screenshot export failed: {}", e.message()), true);
            return false;
        }
    }

    bool EditorScene::requestExportScreenshot(Editor::EditorApp& app)
    {
        if (!pickScreenshotPath_)
            return false;
        try
        {
            const auto path = pickScreenshotPath_(app);
            if (!path)
                return false;
            return exportScreenshot(app, *path);
        }
        catch (const Error::Exception& e)
        {
            controller_.operations().setStatus(fmt::format("Screenshot export failed: {}", e.message()), true);
            return false;
        }
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
        materialPanel(app);
        if (workspace_.consumeJustBuilt())
        {
            ImGui::SetWindowFocus("Objects");
            ImGui::SetWindowFocus("Properties");
        }
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
            if (ImGui::MenuItem("Export Screenshot..."))
                requestExportScreenshot(app);
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
            if (ImGui::MenuItem("Material", nullptr, &materialOpen_) && materialOpen_)
                focusMaterialNextFrame_ = true;
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
