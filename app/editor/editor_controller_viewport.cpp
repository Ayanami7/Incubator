#include "editor_controller.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "editor_app.h"
#include "runtime/model/prepared_model.h"
#include "runtime/scene/scene.h"

namespace Incubator::Editor
{
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

    void EditorController::cancelGizmoDrag(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        hoveredGizmoAxis_ = GizmoAxis::None;
        if (!gizmoDrag_.active)
            return;
        gizmoDrag_ = {};
        operations_.cancelTransaction(scene, clearColor);
    }

    void EditorController::syncViewportExtent(Render::FrameExtent extent) noexcept
    {
        if (extent.width > 0 && extent.height > 0)
            extent_ = extent;
    }

}  // namespace Incubator::Editor
