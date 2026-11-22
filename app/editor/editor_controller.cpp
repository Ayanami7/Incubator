#include "editor_controller.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

#include "editor_app.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/model/prepared_model.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/resource_manager.h"

namespace Incubator::Editor
{
    void EditorController::initialize(EditorApp& app, std::string_view path)
    {
        operations_.initialize(app.renderer());
        if (!path.empty())
            load(app, path, true);
        browseDirectory();
    }

    void EditorController::initialize(Render::Renderer& renderer)
    {
        operations_.initialize(renderer);
        browseDirectory();
    }

    Scene::EntityId EditorController::createEntity(Scene::Scene& scene, const std::array<float, 4>& clearColor)
    {
        return addModel(scene, clearColor, creationPath(useObj_), initialTransform(useObj_))
                   ? operations_.selected()
                   : Scene::EntityId{};
    }

    Scene::EntityId EditorController::createEntity(EditorApp& app)
    {
        return createEntity(app.scene(), app.clearColor());
    }

    bool EditorController::browseDirectory(std::string_view path)
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

    bool EditorController::parentDirectory()
    {
        const auto slash = assetDirectory_.find_last_of('/');
        return browseDirectory(slash == std::string::npos ? std::string{} : assetDirectory_.substr(0, slash));
    }

    bool EditorController::addModel(Scene::Scene& scene, const std::array<float, 4>& clearColor, std::string_view path,
                                    const Scene::Transform& transform)
    {
        auto mutableColor = clearColor;
        cancelGizmoDrag(scene, mutableColor);
        return operations_.addModel(scene, clearColor, path, transform);
    }

    bool EditorController::addModel(EditorApp& app, std::string_view path, const Scene::Transform& transform)
    {
        return addModel(app.scene(), app.clearColor(), path, transform);
    }

    bool EditorController::save(const Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                FileIO::FileIO* fileIO, std::string_view path)
    {
        if (!fileIO)
            return false;
        return operations_.save(*fileIO, scene, clearColor, path);
    }

    bool EditorController::save(EditorApp& app, std::string_view path)
    {
        return save(app.scene(), app.clearColor(), &app.fileIO(), path);
    }

    bool EditorController::save(const Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                FileIO::FileIO* fileIO)
    {
        if (operations_.currentFile().empty())
        {
            beginSaveAs();
            return false;
        }
        return save(scene, clearColor, fileIO, operations_.currentFile());
    }

    bool EditorController::save(EditorApp& app)
    {
        return save(app.scene(), app.clearColor(), &app.fileIO());
    }

    bool EditorController::load(std::unique_ptr<Scene::Scene>& scene, std::array<float, 4>& clearColor,
                                FileIO::FileIO* fileIO, std::string_view path, bool rethrow)
    {
        if (!fileIO)
            return false;
        auto rebuilt = operations_.load(*fileIO, path, rethrow);
        if (!rebuilt)
            return false;
        scene = std::move(rebuilt->scene);
        clearColor = rebuilt->clearColor;
        cancelInteraction(*scene, clearColor, false, true);
        cancelPendingAction();
        orbit_.synchronize(scene->camera());
        scenePath_ = pathBuffer(path);
        return true;
    }

    bool EditorController::load(EditorApp& app, std::string_view path, bool rethrow)
    {
        auto rebuilt = operations_.load(app.fileIO(), path, rethrow);
        if (!rebuilt)
            return false;
        app.setScene(std::move(rebuilt->scene));
        app.clearColor() = rebuilt->clearColor;
        cancelInteraction(app);
        cancelPendingAction();
        orbit_.synchronize(app.scene().camera());
        scenePath_ = pathBuffer(path);
        return true;
    }

    void EditorController::beginSaveAs() noexcept
    {
        fileAction_ = FileAction::SaveAs;
    }

    void EditorController::beginOpen() noexcept
    {
        fileAction_ = FileAction::Open;
    }

    void EditorController::cancelFileAction() noexcept
    {
        fileAction_ = FileAction::None;
    }

    bool EditorController::consumeSaveRequested() noexcept
    {
        if (!saveRequested_)
            return false;
        saveRequested_ = false;
        return true;
    }

    bool EditorController::requestOpen(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        if (!settleForDirtyCheck(scene, clearColor))
        {
            beginOpen();
            return false;
        }
        pendingAction_ = PendingAction::Open;
        pendingSaveAs_ = false;
        return true;
    }

    void EditorController::requestOpen(EditorApp& app)
    {
        requestOpen(app.scene(), app.clearColor());
    }

    bool EditorController::requestClear(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        if (!settleForDirtyCheck(scene, clearColor))
        {
            clear(scene, clearColor);
            return false;
        }
        pendingAction_ = PendingAction::Clear;
        pendingSaveAs_ = false;
        return true;
    }

    void EditorController::requestClear(EditorApp& app)
    {
        requestClear(app.scene(), app.clearColor());
    }

    bool EditorController::requestExit(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        if (!settleForDirtyCheck(scene, clearColor))
        {
            exitRequested_ = true;
            return true;
        }
        pendingAction_ = PendingAction::Exit;
        pendingSaveAs_ = false;
        return false;
    }

    void EditorController::requestExit(EditorApp& app)
    {
        if (requestExit(app.scene(), app.clearColor()))
        {
            clearExitRequested();
            app.requestExit();
        }
    }

    bool EditorController::requestClose(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        if (!settleForDirtyCheck(scene, clearColor))
            return false;
        pendingAction_ = PendingAction::Exit;
        pendingSaveAs_ = false;
        return true;
    }

    void EditorController::requestClose(EditorApp& app)
    {
        if (requestClose(app.scene(), app.clearColor()))
            app.window().setShouldClose(false);
    }

    bool EditorController::resolveUnsavedSave(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                              FileIO::FileIO* fileIO)
    {
        if (!operations_.currentFile().empty())
        {
            if (save(scene, clearColor, fileIO))
                executePendingAction(scene, clearColor);
            cancelPendingAction();
            return exitRequested_;
        }
        pendingSaveAs_ = true;
        beginSaveAs();
        return false;
    }

    bool EditorController::resolveUnsavedSave(EditorApp& app)
    {
        if (resolveUnsavedSave(app.scene(), app.clearColor(), &app.fileIO()))
        {
            clearExitRequested();
            app.requestExit();
            return true;
        }
        return false;
    }

    bool EditorController::discardPendingAction(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        executePendingAction(scene, clearColor);
        cancelPendingAction();
        return exitRequested_;
    }

    bool EditorController::discardPendingAction(EditorApp& app)
    {
        if (discardPendingAction(app.scene(), app.clearColor()))
        {
            clearExitRequested();
            app.requestExit();
            return true;
        }
        return false;
    }

    void EditorController::cancelPendingAction() noexcept
    {
        pendingAction_ = PendingAction::None;
        pendingSaveAs_ = false;
    }

    void EditorController::operationPath(std::string_view path)
    {
        scenePath_ = pathBuffer(path);
    }

    void EditorController::refreshSceneFiles(FileIO::FileIO& fileIO)
    {
        sceneFiles_.clear();
        std::vector<std::string> scanDirs;
        scanDirs.emplace_back("assets/scenes");
        if (!operations_.currentFile().empty() && Utils::Utf8::isValid(operations_.currentFile()))
        {
            const std::filesystem::path currentParent =
                Utils::Utf8::toFsPath(operations_.currentFile()).parent_path();
            if (!currentParent.empty() && !currentParent.is_absolute())
            {
                std::string parentStr = Utils::Utf8::toUtf8String(currentParent);
                std::replace(parentStr.begin(), parentStr.end(), '\\', '/');
                if (parentStr != "assets/scenes")
                    scanDirs.push_back(std::move(parentStr));
            }
        }

        for (const auto& dir : scanDirs)
        {
            try
            {
                const std::filesystem::path dirPath = Utils::Utf8::toFsPath(dir);
                if (!fileIO.directoryExists(dirPath))
                    continue;
                for (const auto& entry : fileIO.listDirectory(dirPath))
                {
                    if (entry.directory || !Utils::Utf8::isValid(entry.name))
                        continue;
                    const std::filesystem::path entryPath = Utils::Utf8::toFsPath(entry.name);
                    std::string ext = Utils::Utf8::toUtf8String(entryPath.extension());
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (ext != ".json")
                        continue;
                    std::string normalized = Utils::Utf8::toUtf8String(dirPath / entryPath);
                    std::replace(normalized.begin(), normalized.end(), '\\', '/');
                    sceneFiles_.push_back(std::move(normalized));
                }
            }
            catch (...)
            {
                // 静默容错：目录不存在或枚举失败时不改写 operations_.message() / messageError()。
            }
        }
        std::sort(sceneFiles_.begin(), sceneFiles_.end());
        sceneFiles_.erase(std::unique(sceneFiles_.begin(), sceneFiles_.end()), sceneFiles_.end());
    }

    void EditorController::refreshSceneFiles(EditorApp& app)
    {
        refreshSceneFiles(app.fileIO());
    }

    bool EditorController::confirmFileAction(std::unique_ptr<Scene::Scene>& scene, std::array<float, 4>& clearColor,
                                             FileIO::FileIO* fileIO)
    {
        const std::string path(scenePath_.data());
        const bool wasSaveAs = fileAction_ == FileAction::SaveAs;
        const bool success = fileAction_ == FileAction::Open
                                 ? load(scene, clearColor, fileIO, path)
                                 : (fileAction_ == FileAction::SaveAs && scene ? save(*scene, clearColor, fileIO, path)
                                                                               : false);
        if (success)
            fileAction_ = FileAction::None;
        if (wasSaveAs && pendingSaveAs_)
        {
            if (success && scene)
                executePendingAction(*scene, clearColor);
            cancelPendingAction();
        }
        return success;
    }

    bool EditorController::confirmFileAction(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                             FileIO::FileIO* fileIO)
    {
        if (fileAction_ != FileAction::SaveAs)
            return false;
        const std::string path(scenePath_.data());
        const bool success = save(scene, clearColor, fileIO, path);
        if (success)
            fileAction_ = FileAction::None;
        if (pendingSaveAs_)
        {
            if (success)
                executePendingAction(scene, clearColor);
            cancelPendingAction();
        }
        return success;
    }

    bool EditorController::confirmFileAction(EditorApp& app)
    {
        const std::string path(scenePath_.data());
        const bool wasSaveAs = fileAction_ == FileAction::SaveAs;
        const bool success = fileAction_ == FileAction::Open     ? load(app, path)
                             : fileAction_ == FileAction::SaveAs ? save(app, path)
                                                                 : false;
        if (success)
            fileAction_ = FileAction::None;
        if (wasSaveAs && pendingSaveAs_)
        {
            if (success)
                executePendingAction(app.scene(), app.clearColor());
            cancelPendingAction();
            if (exitRequested_)
            {
                clearExitRequested();
                app.requestExit();
            }
        }
        return success;
    }

    bool EditorController::undo(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        cancelGizmoDrag(scene, clearColor);
        if (!operations_.undo(scene, clearColor))
            return false;
        activePanelTransaction_ = PanelTransactionKind::None;
        clickCandidate_ = false;
        orbit_.synchronize(scene.camera());
        return true;
    }

    bool EditorController::undo(EditorApp& app)
    {
        return undo(app.scene(), app.clearColor());
    }

    bool EditorController::redo(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        cancelGizmoDrag(scene, clearColor);
        if (!operations_.redo(scene, clearColor))
            return false;
        activePanelTransaction_ = PanelTransactionKind::None;
        clickCandidate_ = false;
        orbit_.synchronize(scene.camera());
        return true;
    }

    bool EditorController::redo(EditorApp& app)
    {
        return redo(app.scene(), app.clearColor());
    }

    std::string EditorController::statusText() const
    {
        std::string text = operations_.currentFile().empty() ? "(unsaved)" : operations_.currentFile();
        if (operations_.isDirty())
            text += " *";
        text += " | ";
        text += operations_.message();
        return text;
    }

    void EditorController::select(Scene::EntityId id)
    {
        if (id != operations_.selected())
        {
            gizmoDrag_ = {};
            hoveredGizmoAxis_ = GizmoAxis::None;
        }
        operations_.select(id);
    }

    void EditorController::select(Scene::Scene& scene, std::array<float, 4>& clearColor, Scene::EntityId id)
    {
        if (id != operations_.selected())
            cancelGizmoDrag(scene, clearColor);
        operations_.select(id);
    }

    void EditorController::select(EditorApp& app, Scene::EntityId id)
    {
        select(app.scene(), app.clearColor(), id);
    }

    void EditorController::setGizmoMode(GizmoMode mode) noexcept
    {
        // 拖拽进行中锁定模式，避免在单次几何求解过程中切换分支或坐标轴定义。
        if (gizmoDrag_.active)
            return;
        gizmoMode_ = mode;
    }

    void EditorController::deleteSelected(Scene::Scene& scene, const std::array<float, 4>& clearColor)
    {
        auto mutableColor = clearColor;
        cancelGizmoDrag(scene, mutableColor);
        operations_.remove(scene, clearColor);
        clickCandidate_ = false;
    }

    void EditorController::deleteSelected(EditorApp& app)
    {
        deleteSelected(app.scene(), app.clearColor());
    }

    bool EditorController::setVisible(Scene::Scene& scene, const std::array<float, 4>& clearColor, Scene::EntityId id,
                                      bool visible)
    {
        return operations_.setVisible(scene, clearColor, id, visible);
    }

    bool EditorController::setVisible(EditorApp& app, Scene::EntityId id, bool visible)
    {
        return setVisible(app.scene(), app.clearColor(), id, visible);
    }

    bool EditorController::setParent(Scene::Scene& scene, std::array<float, 4>& clearColor, Scene::EntityId id,
                                     Scene::EntityId parent)
    {
        cancelGizmoDrag(scene, clearColor);
        commitPanelTransactions(scene, clearColor);
        return operations_.setParent(scene, clearColor, id, parent);
    }

    bool EditorController::setParent(EditorApp& app, Scene::EntityId id, Scene::EntityId parent)
    {
        return setParent(app.scene(), app.clearColor(), id, parent);
    }

    bool EditorController::setEntityShading(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                            Render::ShadingMode shading)
    {
        cancelGizmoDrag(scene, clearColor);
        commitPanelTransactions(scene, clearColor);
        return operations_.setEntityShading(scene, clearColor, shading);
    }

    bool EditorController::setEntityShading(EditorApp& app, Render::ShadingMode shading)
    {
        return setEntityShading(app.scene(), app.clearColor(), shading);
    }

    bool EditorController::setDisplay(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                      Render::DisplaySettings display)
    {
        cancelGizmoDrag(scene, clearColor);
        commitPanelTransactions(scene, clearColor);
        return operations_.setDisplay(scene, clearColor, display);
    }

    bool EditorController::setDisplay(EditorApp& app, Render::DisplaySettings display)
    {
        return setDisplay(app.scene(), app.clearColor(), display);
    }

    Scene::EntityId EditorController::duplicateSelected(Scene::Scene& scene, const std::array<float, 4>& clearColor)
    {
        auto mutableColor = clearColor;
        cancelGizmoDrag(scene, mutableColor);
        return operations_.duplicate(scene, clearColor);
    }

    Scene::EntityId EditorController::duplicateSelected(EditorApp& app)
    {
        return duplicateSelected(app.scene(), app.clearColor());
    }

    void EditorController::clear(Scene::Scene& scene, const std::array<float, 4>& clearColor)
    {
        auto mutableColor = clearColor;
        cancelGizmoDrag(scene, mutableColor);
        operations_.clear(scene, clearColor);
        clickCandidate_ = false;
    }

    void EditorController::clear(EditorApp& app)
    {
        clear(app.scene(), app.clearColor());
    }

    bool EditorController::beginPanelTransaction(Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                                 PanelTransactionKind kind)
    {
        if (kind == PanelTransactionKind::None)
            return false;
        if (activePanelTransaction_ != PanelTransactionKind::None && activePanelTransaction_ != kind)
            commitPanelTransactions(scene, clearColor);
        if (!operations_.inTransaction())
        {
            operations_.beginTransaction(scene, clearColor);
            activePanelTransaction_ = operations_.inTransaction() ? kind : PanelTransactionKind::None;
        }
        return activePanelTransaction_ == kind;
    }

    bool EditorController::beginPanelTransaction(EditorApp& app, PanelTransactionKind kind)
    {
        return beginPanelTransaction(app.scene(), app.clearColor(), kind);
    }

    void EditorController::commitPanelTransactions(Scene::Scene& scene, const std::array<float, 4>& clearColor)
    {
        if (activePanelTransaction_ == PanelTransactionKind::None)
            return;
        activePanelTransaction_ = PanelTransactionKind::None;
        operations_.commitTransaction(scene, clearColor);
    }

    void EditorController::commitPanelTransactions(EditorApp& app)
    {
        commitPanelTransactions(app.scene(), app.clearColor());
    }

    void EditorController::cancelPanelTransactions(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        if (activePanelTransaction_ == PanelTransactionKind::None)
            return;
        activePanelTransaction_ = PanelTransactionKind::None;
        operations_.cancelTransaction(scene, clearColor);
    }

    void EditorController::cancelPanelTransactions(EditorApp& app)
    {
        cancelPanelTransactions(app.scene(), app.clearColor());
    }

    void EditorController::syncPanelTransactionState() noexcept
    {
        if (!operations_.inTransaction())
            activePanelTransaction_ = PanelTransactionKind::None;
    }

    void EditorController::cancelInteraction(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                             bool windowUnavailable, bool viewportVisible)
    {
        orbit_.cancel();
        clickCandidate_ = false;
        const bool wasGizmoDragging = gizmoDrag_.active;
        gizmoDrag_ = {};
        hoveredGizmoAxis_ = GizmoAxis::None;
        extent_ = {0, 0};
        if (windowUnavailable)
            shortcutSuspended_ = true;
        // 仅因 Scene 视口折叠或关闭触发的取消只重置视口手势（含 Gizmo 拖拽），保留 Properties 面板正在进行的属性事务。
        if (windowUnavailable || viewportVisible || wasGizmoDragging)
        {
            activePanelTransaction_ = PanelTransactionKind::None;
            operations_.cancelTransaction(scene, clearColor);
        }
        else if (!operations_.inTransaction())
        {
            activePanelTransaction_ = PanelTransactionKind::None;
        }
    }

    void EditorController::cancelInteraction(EditorApp& app)
    {
        int fbWidth = 0;
        int fbHeight = 0;
        app.window().getFramebufferSize(fbWidth, fbHeight);
        const auto& raw = app.window().input();
        const bool windowUnavailable = !raw.focused() || raw.focusLost() || fbWidth <= 0 || fbHeight <= 0;
        const bool viewportVisible = app.sceneExtent().width > 0 && app.sceneExtent().height > 0;
        cancelInteraction(app.scene(), app.clearColor(), windowUnavailable, viewportVisible);
    }

    bool EditorController::focusSelected(Scene::Scene& scene, Render::FrameExtent extent)
    {
        const auto* entity = scene.find(operations_.selected());
        if (!entity || !entity->model || !entity->model->bounds())
            return false;
        const auto world = scene.worldMatrix(entity->id);
        if (!world)
            return false;
        const auto bounds = entity->model->bounds()->transformed(*world);
        auto camera = scene.camera();
        if (!orbit_.focus(camera, bounds, extent))
            return false;
        scene.setCamera(camera);
        return true;
    }

    bool EditorController::focusSelected(EditorApp& app, Render::FrameExtent extent)
    {
        return focusSelected(app.scene(), extent);
    }

    void EditorController::input(Scene::Scene& scene, std::array<float, 4>& clearColor, Render::FrameExtent extent,
                                 const App::ViewportRegion& region, Input::PointerValue windowSize,
                                 const App::UiInput& uiInput, float dragThreshold)
    {
        extent_ = extent;
        auto camera = scene.camera();
        const auto logicalSize = region.size.x > 0 && region.size.y > 0 ? region.size : windowSize;
        const glm::vec2 cursorScreenPos(static_cast<float>(region.origin.x + uiInput.position.x),
                                        static_cast<float>(region.origin.y + uiInput.position.y));
        const bool cameraGesture = uiInput.rotate.down || uiInput.rotate.pressed || uiInput.rotate.released ||
                                   uiInput.pan.down || uiInput.pan.pressed || uiInput.pan.released ||
                                   uiInput.scroll.y != 0 || orbit_.dragging();

        if (gizmoDrag_.active)
        {
            if (gizmoDrag_.entity != operations_.selected() || !scene.find(gizmoDrag_.entity))
            {
                cancelGizmoDrag(scene, clearColor);
                return;
            }
            hoveredGizmoAxis_ = gizmoDrag_.axis;
            if (uiInput.select.down)
            {
                const auto updated = updateGizmoDrag(gizmoDrag_, camera, extent, region, cursorScreenPos);
                operations_.setTransform(scene, clearColor, updated);
            }
            if (uiInput.select.released || !uiInput.select.down)
            {
                gizmoDrag_ = {};
                operations_.commitTransaction(scene, clearColor);
                if (!cameraGesture && !uiInput.captureMouse && operations_.selected().valid())
                {
                    const auto gizmo = projectGizmo(scene, operations_.selected(), gizmoMode_, extent, region);
                    hoveredGizmoAxis_ = pickGizmoAxis(cursorScreenPos, gizmo);
                }
                else
                {
                    hoveredGizmoAxis_ = GizmoAxis::None;
                }
            }
            // Gizmo 拖拽活跃帧独占视口交互，屏蔽左键点选、相机轨道旋转/平移/滚轮缩放与 F 键聚焦。
            return;
        }

        if (!cameraGesture && !uiInput.captureMouse && operations_.selected().valid())
        {
            const auto gizmo = projectGizmo(scene, operations_.selected(), gizmoMode_, extent, region);
            hoveredGizmoAxis_ = pickGizmoAxis(cursorScreenPos, gizmo);
        }
        else
        {
            hoveredGizmoAxis_ = GizmoAxis::None;
        }

        if (uiInput.select.pressed && !uiInput.captureMouse && !cameraGesture)
        {
            if (hoveredGizmoAxis_ != GizmoAxis::None)
            {
                if (const auto* entity = scene.find(operations_.selected()))
                {
                    bool canDrag = true;
                    std::optional<glm::vec3> worldOrigin;
                    glm::dmat4 inverseParentWorld(1.0);
                    if (entity->parent.valid())
                    {
                        if (gizmoMode_ != GizmoMode::Translate)
                        {
                            canDrag = false;
                        }
                        else
                        {
                            const auto parentWorld = scene.worldMatrix(entity->parent);
                            const auto entityWorld = scene.worldMatrix(entity->id);
                            if (!parentWorld || !entityWorld)
                            {
                                canDrag = false;
                            }
                            else
                            {
                                const double det = glm::determinant(glm::dmat3(*parentWorld));
                                if (!std::isfinite(det) || std::abs(det) <= 1e-8)
                                {
                                    canDrag = false;
                                }
                                else
                                {
                                    worldOrigin = glm::vec3((*entityWorld)[3]);
                                    inverseParentWorld = glm::inverse(glm::dmat4(*parentWorld));
                                }
                            }
                        }
                    }
                    const auto drag = canDrag
                                          ? beginGizmoDrag(operations_.selected(), gizmoMode_, hoveredGizmoAxis_,
                                                           entity->transform, camera, extent, region, cursorScreenPos,
                                                           90.0f, worldOrigin, inverseParentWorld)
                                          : GizmoDragState{};
                    if (drag.active)
                    {
                        commitPanelTransactions(scene, clearColor);
                        operations_.beginTransaction(scene, clearColor);
                        gizmoDrag_ = drag;
                        clickCandidate_ = false;
                    }
                    else
                    {
                        clickCandidate_ = true;
                    }
                }
                else
                {
                    clickCandidate_ = true;
                }
            }
            else
            {
                clickCandidate_ = true;
            }
        }
        if (clickCandidate_ &&
            (uiInput.captureMouse || cameraGesture || uiInput.selectDragDistanceSquared > dragThreshold * dragThreshold))
            clickCandidate_ = false;
        if (clickCandidate_ && uiInput.select.released)
        {
            const glm::vec2 position(static_cast<float>(uiInput.position.x / region.size.x),
                                     static_cast<float>(uiInput.position.y / region.size.y));
            const auto hit = scene.pick(extent, position);
            operations_.select(hit ? hit->id : Scene::EntityId{});
            clickCandidate_ = false;
        }
        else if (clickCandidate_ && !uiInput.select.down && !uiInput.select.pressed)
            clickCandidate_ = false;
        if (orbit_.apply(camera, uiInput, logicalSize))
            scene.setCamera(camera);
        if (uiInput.focusPressed && !uiInput.captureKeyboard)
            focusSelected(scene, extent);
    }

    void EditorController::input(EditorApp& app, Render::FrameExtent extent, const App::UiInput& uiInput,
                                 float dragThreshold)
    {
        int width = 0;
        int height = 0;
        app.window().getWindowSize(width, height);
        input(app.scene(), app.clearColor(), extent, app.viewportRegion(), {double(width), double(height)}, uiInput,
              dragThreshold);
    }

    void EditorController::resetCamera(Scene::Scene& scene)
    {
        scene.setCamera(Scene::Camera({0, 0, 6}));
        orbit_.synchronize(scene.camera());
    }

    void EditorController::resetCamera(EditorApp& app)
    {
        resetCamera(app.scene());
    }

    void EditorController::handleShortcuts(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                           const ShortcutInput& state)
    {
        if (!state.focused || state.focusLost)
        {
            shortcutSuspended_ = true;
            return;
        }
        if (shortcutSuspended_)
        {
            if (state.inputQueueEmpty && !state.wDown && !state.eDown && !state.rDown && !state.zDown && !state.yDown)
                shortcutSuspended_ = false;
            return;
        }
        if (state.wantTextInput || fileAction_ != FileAction::None || pendingAction_ != PendingAction::None ||
            state.popupOpen || operations_.inTransaction() || orbit_.dragging() || gizmoDrag_.active)
            return;
        if (state.keyAlt || state.keySuper)
            return;
        if (state.keyCtrl)
        {
            if (!state.keyShift && state.zPressed)
                undo(scene, clearColor);
            else if ((!state.keyShift && state.yPressed) || (state.keyShift && state.zPressed))
                redo(scene, clearColor);
            return;
        }
        if (state.mouseRightDown || state.mouseMiddleDown)
            return;
        if (state.wPressed)
            setGizmoMode(GizmoMode::Translate);
        else if (state.ePressed)
            setGizmoMode(GizmoMode::Rotate);
        else if (state.rPressed)
            setGizmoMode(GizmoMode::Scale);
    }

    void EditorController::handleShortcuts(EditorApp& app, const ShortcutInput& state)
    {
        handleShortcuts(app.scene(), app.clearColor(), state);
    }

    ViewportOverlay EditorController::buildViewportOverlay(const Scene::Scene& scene, Render::FrameExtent extent,
                                                           const App::ViewportRegion& region) const
    {
        ViewportOverlay overlay;
        overlay.clipMin = {static_cast<float>(region.origin.x), static_cast<float>(region.origin.y)};
        overlay.clipMax = {static_cast<float>(region.origin.x + region.size.x),
                           static_cast<float>(region.origin.y + region.size.y)};

        const auto boxSegments = projectSelectionBox(scene, operations_.selected(), extent, region);
        const auto gizmo = projectGizmo(scene, operations_.selected(), gizmoMode_, extent, region);
        if (boxSegments.empty() && gizmo.segments.empty())
            return overlay;

        constexpr OverlayColor kBoxColor{255, 200, 64, 255};
        constexpr OverlayColor kHighlightColor{255, 240, 100, 255};
        const GizmoAxis highlightedAxis = gizmoDrag_.active ? gizmoDrag_.axis : hoveredGizmoAxis_;
        const auto axisDefaultColor = [](GizmoAxis axis) -> OverlayColor {
            switch (axis)
            {
                case GizmoAxis::X: return {230, 70, 70, 255};
                case GizmoAxis::Y: return {70, 210, 90, 255};
                case GizmoAxis::Z: return {70, 140, 245, 255};
                case GizmoAxis::None: break;
            }
            return {255, 255, 255, 255};
        };

        overlay.lines.reserve(boxSegments.size() + gizmo.segments.size());
        for (const auto& segment : boxSegments)
            overlay.lines.push_back({segment, kBoxColor, 2.0f});
        for (const bool drawHighlighted : {false, true})
        {
            for (const auto& gizmoSegment : gizmo.segments)
            {
                const bool highlighted =
                    gizmoSegment.axis != GizmoAxis::None && gizmoSegment.axis == highlightedAxis;
                if (highlighted != drawHighlighted)
                    continue;
                const OverlayColor color = highlighted ? kHighlightColor : axisDefaultColor(gizmoSegment.axis);
                const float thickness = highlighted ? 3.5f : 2.5f;
                overlay.lines.push_back({gizmoSegment.segment, color, thickness});
            }
        }
        return overlay;
    }

    ViewportOverlay EditorController::buildViewportOverlay(EditorApp& app) const
    {
        return buildViewportOverlay(app.scene(), app.sceneExtent(), app.viewportRegion());
    }

    void EditorController::executePendingAction(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        switch (pendingAction_)
        {
            case PendingAction::Open:
                beginOpen();
                break;
            case PendingAction::Clear:
                clear(scene, clearColor);
                break;
            case PendingAction::Exit:
                exitRequested_ = true;
                break;
            case PendingAction::None:
                break;
        }
    }

    void EditorController::cancelGizmoDrag(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        hoveredGizmoAxis_ = GizmoAxis::None;
        if (!gizmoDrag_.active)
            return;
        gizmoDrag_ = {};
        operations_.cancelTransaction(scene, clearColor);
    }

    bool EditorController::settleForDirtyCheck(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        if (operations_.inTransaction())
        {
            gizmoDrag_ = {};
            hoveredGizmoAxis_ = GizmoAxis::None;
            activePanelTransaction_ = PanelTransactionKind::None;
            operations_.commitTransaction(scene, clearColor);
        }
        return operations_.isDirty();
    }

    void EditorController::syncViewportExtent(Render::FrameExtent extent) noexcept
    {
        if (extent.width > 0 && extent.height > 0)
            extent_ = extent;
    }

    std::vector<char> EditorController::pathBuffer(std::string_view path)
    {
        std::vector<char> buffer(std::max(size_t{1024}, path.size() + 1), '\0');
        std::copy(path.begin(), path.end(), buffer.begin());
        return buffer;
    }

    std::string_view EditorController::creationPath(bool useObj)
    {
        return useObj ? "models/utah_teapot/utah_teapot.obj" : "models/avocado/glTF/Avocado.gltf";
    }

    Scene::Transform EditorController::initialTransform(bool useObj)
    {
        return Scene::Transform({0, useObj ? 0.0f : -0.78f, 0}, {1, 0, 0, 0}, glm::vec3(useObj ? 0.075f : 24.0f));
    }
}  // namespace Incubator::Editor
