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
#include "runtime/resource/asset_importer.h"
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

    bool EditorController::setMaterialOverride(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                               const Scene::MaterialOverride& overrideValue)
    {
        cancelGizmoDrag(scene, clearColor);
        commitPanelTransactions(scene, clearColor);
        return operations_.setMaterialOverride(scene, clearColor, overrideValue);
    }

    bool EditorController::setMaterialOverride(EditorApp& app, const Scene::MaterialOverride& overrideValue)
    {
        return setMaterialOverride(app.scene(), app.clearColor(), overrideValue);
    }

    bool EditorController::resetMaterialOverride(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                                 uint32_t materialSlot)
    {
        cancelGizmoDrag(scene, clearColor);
        commitPanelTransactions(scene, clearColor);
        return operations_.resetMaterialOverride(scene, clearColor, materialSlot);
    }

    bool EditorController::resetMaterialOverride(EditorApp& app, uint32_t materialSlot)
    {
        return resetMaterialOverride(app.scene(), app.clearColor(), materialSlot);
    }

    bool EditorController::resetAllMaterialOverrides(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        cancelGizmoDrag(scene, clearColor);
        commitPanelTransactions(scene, clearColor);
        return operations_.resetAllMaterialOverrides(scene, clearColor);
    }

    bool EditorController::resetAllMaterialOverrides(EditorApp& app)
    {
        return resetAllMaterialOverrides(app.scene(), app.clearColor());
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

    bool EditorController::setLighting(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                       Render::Lighting lighting)
    {
        cancelGizmoDrag(scene, clearColor);
        commitPanelTransactions(scene, clearColor);
        return operations_.setLighting(scene, clearColor, std::move(lighting));
    }

    bool EditorController::setLighting(EditorApp& app, Render::Lighting lighting)
    {
        return setLighting(app.scene(), app.clearColor(), std::move(lighting));
    }

    bool EditorController::addLocalLight(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                         Render::LocalLightType type)
    {
        cancelGizmoDrag(scene, clearColor);
        commitPanelTransactions(scene, clearColor);
        auto lighting = scene.lighting();
        if (lighting.localLights.size() >= Render::MAX_LOCAL_LIGHTS)
            return false;
        Render::LocalLight light;
        light.type = type;
        lighting.localLights.push_back(light);
        return operations_.setLighting(scene, clearColor, std::move(lighting));
    }

    bool EditorController::addLocalLight(EditorApp& app, Render::LocalLightType type)
    {
        return addLocalLight(app.scene(), app.clearColor(), type);
    }

    bool EditorController::removeLocalLight(Scene::Scene& scene, std::array<float, 4>& clearColor, size_t index)
    {
        cancelGizmoDrag(scene, clearColor);
        commitPanelTransactions(scene, clearColor);
        auto lighting = scene.lighting();
        if (index >= lighting.localLights.size())
            return false;
        lighting.localLights.erase(lighting.localLights.begin() + static_cast<ptrdiff_t>(index));
        return operations_.setLighting(scene, clearColor, std::move(lighting));
    }

    bool EditorController::removeLocalLight(EditorApp& app, size_t index)
    {
        return removeLocalLight(app.scene(), app.clearColor(), index);
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
