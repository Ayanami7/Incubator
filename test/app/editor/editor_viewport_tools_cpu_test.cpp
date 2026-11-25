// 编译期确认控制器、操作接口与 Gizmo 头文件独立于 ImGui 与 Vulkan。
#include "app/editor/editor_controller.h"
#include "app/editor/editor_operations.h"
#include "app/editor/gizmo.h"
#include "app/editor/viewport_debug_lines.h"
#include "app/editor/viewport_screenshot.h"
#include "runtime/resource/asset_importer.h"

#ifdef IMGUI_VERSION
#error "editor_controller.h and viewport_input.h must not include imgui.h"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glm/ext/matrix_transform.hpp>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "app/common/viewport_input.h"
#include "app/editor/selection_overlay.h"
#include "runtime/core/error/error.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/asset_source.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/scene/scene.h"

namespace
{
    using namespace Incubator;
    namespace fs = std::filesystem;


    TEST(EditorSelectionOverlayTest, ClipHomogeneousSegmentHandlesNearFarBehindCameraOffscreenAndNonFinite)
    {
        const auto inside = Editor::clipHomogeneousSegment({-0.5, 0.25, 0.5, 2.0}, {0.5, -0.25, 1.5, 2.0});
        ASSERT_TRUE(inside.has_value());
        EXPECT_TRUE(inside->first.x == -0.5 && inside->second.z == 1.5);
        EXPECT_FALSE(Editor::clipHomogeneousSegment({0.0, 0.0, -1.0, -2.0}, {0.1, 0.1, -0.5, -1.0}).has_value() ||
                     Editor::clipHomogeneousSegment({3.0, 0.0, 0.5, 2.0}, {4.0, 0.0, 0.5, 2.0}).has_value() ||
                     Editor::clipHomogeneousSegment({0.0, 0.0, 2.5, 2.0}, {0.0, 0.0, 3.0, 2.0}).has_value());

        const auto acrossNear = Editor::clipHomogeneousSegment({0.0, 0.0, -1.0, -1.0}, {0.0, 0.0, 1.0, 3.0});
        ASSERT_TRUE(acrossNear.has_value() && acrossNear->first.w > 0.0 && acrossNear->second.w > 0.0);
        EXPECT_NEAR(acrossNear->first.z, 0.0, 1e-12);
        const auto acrossFar = Editor::clipHomogeneousSegment({0.0, 0.0, 0.5, 2.0}, {0.0, 0.0, 4.0, 2.0});
        ASSERT_TRUE(acrossFar.has_value()); EXPECT_NEAR(acrossFar->second.z, acrossFar->second.w, 1e-12);
        const auto acrossRight = Editor::clipHomogeneousSegment({0.0, 0.0, 0.5, 2.0}, {4.0, 0.0, 0.5, 2.0});
        ASSERT_TRUE(acrossRight.has_value()); EXPECT_NEAR(acrossRight->second.x, acrossRight->second.w, 1e-12);

        const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        EXPECT_FALSE(Editor::clipHomogeneousSegment({nan, 0.0, 0.5, 1.0}, {0.0, 0.0, 0.5, 1.0}).has_value() ||
                     Editor::clipHomogeneousSegment({0.0, 0.0, 0.5, 1.0}, {inf, 0.0, 0.5, 1.0}).has_value());
    }

    TEST(EditorSelectionOverlayTest, ProjectAabbOverlayMatchesViewportMappingAcrossDpiAndClipsBounds)
    {
        const Scene::Camera camera({0.0f, 0.0f, 6.0f});
        const App::ViewportRegion region{{100.0, 50.0}, {800.0, 600.0}, true, true};
        const Render::FrameExtent extent{800, 600};
        const Math::Aabb box{{-1.0f, -0.5f, -1.0f}, {1.0f, 1.5f, 1.0f}};

        EXPECT_TRUE(Editor::projectAabbOverlay(box, camera, {0, 600}, region).empty() &&
                    Editor::projectAabbOverlay(box, camera, extent, {{100.0, 50.0}, {0.0, 600.0}, true, true}).empty());

        const auto segments = Editor::projectAabbOverlay(box, camera, extent, region);
        ASSERT_EQ(segments.size(), 12u);
        float minY = float(region.origin.y + region.size.y), maxY = float(region.origin.y);
        for (const auto& s : segments)
            for (const auto& p : {s.start, s.end})
            {
                EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
                EXPECT_GE(p.x, region.origin.x - 1e-3f); EXPECT_LE(p.x, region.origin.x + region.size.x + 1e-3f);
                EXPECT_GE(p.y, region.origin.y - 1e-3f); EXPECT_LE(p.y, region.origin.y + region.size.y + 1e-3f);
                minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
            }
        const float centerY = float(region.origin.y + region.size.y * 0.5);
        EXPECT_GT(centerY - minY, maxY - centerY);

        for (float dpi : {1.5f, 2.0f})
        {
            const auto scaledSegs = Editor::projectAabbOverlay(box, camera, {uint32_t(extent.width * dpi), uint32_t(extent.height * dpi)}, region);
            ASSERT_EQ(scaledSegs.size(), segments.size());
            for (size_t i = 0; i < segments.size(); ++i)
            {
                EXPECT_NEAR(scaledSegs[i].start.x, segments[i].start.x, 1e-3f); EXPECT_NEAR(scaledSegs[i].start.y, segments[i].start.y, 1e-3f);
                EXPECT_NEAR(scaledSegs[i].end.x, segments[i].end.x, 1e-3f); EXPECT_NEAR(scaledSegs[i].end.y, segments[i].end.y, 1e-3f);
            }
        }
        const auto clipped = Editor::projectAabbOverlay({{-0.5f, -0.5f, 3.0f}, {0.5f, 0.5f, 9.0f}}, camera, extent, region);
        EXPECT_EQ(clipped.size(), 8u);
        for (const auto& s : clipped)
            for (const auto& p : {s.start, s.end})
                EXPECT_TRUE(p.x >= region.origin.x - 1e-3f && p.x <= region.origin.x + region.size.x + 1e-3f &&
                            p.y >= region.origin.y - 1e-3f && p.y <= region.origin.y + region.size.y + 1e-3f);

        EXPECT_TRUE(Editor::projectAabbOverlay({{-2.0f, -2.0f, 3.0f}, {2.0f, 2.0f, 9.0f}}, camera, extent, region).empty() &&
                    Editor::projectAabbOverlay({{-1.0f, -1.0f, 7.0f}, {1.0f, 1.0f, 10.0f}}, camera, extent, region).empty() &&
                    Editor::projectAabbOverlay({{50.0f, 0.0f, -2.0f}, {52.0f, 2.0f, 0.0f}}, camera, extent, region).empty());

        Scene::Scene emptyScene;
        const auto noModel = emptyScene.createEntity();
        EXPECT_TRUE(Editor::projectSelectionBox(emptyScene, {}, extent, region).empty() &&
                    Editor::projectSelectionBox(emptyScene, noModel, extent, region).empty());
    }

    TEST(EditorGizmoTest, ViewDepthConstantScreenScaleAndViewportAlignment)
    {
        Scene::Scene scene;
        auto camera = scene.camera();
        camera.setPosition({0.0f, 0.0f, 5.0f}); camera.setRotation({1.0f, 0.0f, 0.0f, 0.0f});
        camera.setPerspective({glm::radians(60.0f), 0.1f, 100.0f});
        scene.setCamera(camera);

        const auto entityId = scene.createEntity(nullptr, Scene::Transform{}, true, "EmptyPivot");
        const Render::FrameExtent extent{800, 600};
        const App::ViewportRegion region{{100.0, 50.0}, {800.0, 600.0}, true, true};
        const auto yAxisLength = [](const Editor::ProjectedGizmo& gizmo) {
            for (const auto& item : gizmo.segments)
                if (item.axis == Editor::GizmoAxis::Y) return glm::length(item.segment.end - item.segment.start);
            return 0.0f;
        };

        const auto centerGizmo = Editor::projectGizmo(scene, entityId, Editor::GizmoMode::Translate, extent, region, 90.0f);
        ASSERT_TRUE(centerGizmo.visible && centerGizmo.segments.size() == 9u);
        EXPECT_NEAR(yAxisLength(centerGizmo), 90.0f, 1e-2f);

        const auto aabbSegs = Editor::projectAabbOverlay({{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}}, camera, extent, region);
        ASSERT_EQ(aabbSegs.size(), 12u);
        glm::vec2 aabbCenter(0.0f);
        for (const auto& s : aabbSegs) aabbCenter += s.start + s.end;
        aabbCenter /= float(aabbSegs.size() * 2);
        EXPECT_NEAR(centerGizmo.segments.front().segment.start.x, aabbCenter.x, 1e-3f);
        EXPECT_NEAR(centerGizmo.segments.front().segment.start.y, aabbCenter.y, 1e-3f);

        ASSERT_TRUE(scene.setTransform(entityId, Scene::Transform({2.0f, 0.0f, 0.0f})));
        const auto edgeGizmo = Editor::projectGizmo(scene, entityId, Editor::GizmoMode::Translate, extent, region, 90.0f);
        ASSERT_TRUE(edgeGizmo.visible);
        EXPECT_NEAR(edgeGizmo.worldLength, centerGizmo.worldLength, 1e-4f);
        EXPECT_NEAR(yAxisLength(edgeGizmo), 90.0f, 1e-2f);

        ASSERT_TRUE(scene.setTransform(entityId, Scene::Transform{}));
        camera.setPosition({0.0f, 0.0f, 15.0f}); scene.setCamera(camera);
        const auto farGizmo = Editor::projectGizmo(scene, entityId, Editor::GizmoMode::Translate, extent, region, 90.0f);
        ASSERT_TRUE(farGizmo.visible);
        EXPECT_NEAR(farGizmo.worldLength, centerGizmo.worldLength * 3.0f, 1e-3f);
        EXPECT_NEAR(yAxisLength(farGizmo), 90.0f, 1e-2f);

        const auto wideGizmo = Editor::projectGizmo(scene, entityId, Editor::GizmoMode::Translate, {2400, 1200},
                                                    {{100.0, 50.0}, {1200.0, 600.0}, true, true}, 90.0f);
        ASSERT_TRUE(wideGizmo.visible);
        EXPECT_NEAR(yAxisLength(wideGizmo), 90.0f, 1e-2f);
        EXPECT_NEAR(wideGizmo.segments.front().segment.start.x, 700.0f, 1e-2f);
        EXPECT_NEAR(wideGizmo.segments.front().segment.start.y, 350.0f, 1e-2f);
    }

    TEST(EditorGizmoTest, ClippingBehindNearPlaneAndHiddenEntitySafety)
    {
        Scene::Scene scene;
        auto camera = scene.camera();
        camera.setPosition({0.0f, 0.0f, 5.0f}); camera.setRotation({1.0f, 0.0f, 0.0f, 0.0f});
        camera.setPerspective({glm::radians(60.0f), 0.5f, 50.0f});
        scene.setCamera(camera);

        const Render::FrameExtent extent{800, 600};
        const App::ViewportRegion region{{20.0, 10.0}, {800.0, 600.0}, true, true};
        const auto id = scene.createEntity(nullptr, Scene::Transform{}, true, "Target");
        EXPECT_FALSE(Editor::projectGizmo(scene, {}, Editor::GizmoMode::Translate, extent, region).visible ||
                     Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, {0, 600}, region).visible ||
                     Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, {{0.0, 0.0}, {0.0, 100.0}}).visible);

        ASSERT_TRUE(scene.setVisible(id, false));
        EXPECT_FALSE(Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, region).visible);
        ASSERT_TRUE(scene.setVisible(id, true));

        ASSERT_TRUE(scene.setTransform(id, Scene::Transform({0.0f, 0.0f, 4.7f})));
        const auto behindNear = Editor::projectGizmo(scene, id, Editor::GizmoMode::Rotate, extent, region);
        EXPECT_TRUE(!behindNear.visible && behindNear.segments.empty());

        ASSERT_TRUE(scene.setTransform(id, Scene::Transform({0.0f, 0.0f, 8.0f})));
        EXPECT_FALSE(Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, region).visible);

        ASSERT_TRUE(scene.setTransform(id, Scene::Transform({0.0f, 0.0f, 4.45f})));
        const auto acrossNear = Editor::projectGizmo(scene, id, Editor::GizmoMode::Rotate, extent, region, 300.0f);
        ASSERT_TRUE(acrossNear.visible && !acrossNear.segments.empty());
        for (const auto& item : acrossNear.segments)
            for (const auto& p : {item.segment.start, item.segment.end})
                EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && p.x >= region.origin.x - 1e-2f &&
                            p.x <= region.origin.x + region.size.x + 1e-2f && p.y >= region.origin.y - 1e-2f &&
                            p.y <= region.origin.y + region.size.y + 1e-2f);
    }

    TEST(EditorGizmoTest, ModeAxesOrientationAndPickGizmoAxisPriority)
    {
        Scene::Scene scene;
        auto camera = scene.camera();
        camera.setPosition({0.0f, 0.0f, 5.0f}); camera.setRotation({1.0f, 0.0f, 0.0f, 0.0f});
        scene.setCamera(camera);

        const Render::FrameExtent extent{800, 600};
        const App::ViewportRegion region{{0.0, 0.0}, {800.0, 600.0}, true, true};
        const auto id = scene.createEntity(nullptr, Scene::Transform{}, true, "OrientationTarget");
        const auto translateBefore = Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, region);
        const auto rotateBefore = Editor::projectGizmo(scene, id, Editor::GizmoMode::Rotate, extent, region);
        const auto scaleBefore = Editor::projectGizmo(scene, id, Editor::GizmoMode::Scale, extent, region);

        ASSERT_TRUE(scene.setTransform(id, Scene::Transform({0, 0, 0}, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)))));
        const auto translateAfter = Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, region);
        const auto rotateAfter = Editor::projectGizmo(scene, id, Editor::GizmoMode::Rotate, extent, region);
        const auto scaleAfter = Editor::projectGizmo(scene, id, Editor::GizmoMode::Scale, extent, region);

        ASSERT_EQ(translateBefore.segments.size(), translateAfter.segments.size());
        for (size_t i = 0; i < translateBefore.segments.size(); ++i)
        {
            EXPECT_NEAR(translateBefore.segments[i].segment.end.x, translateAfter.segments[i].segment.end.x, 1e-4f);
            EXPECT_NEAR(translateBefore.segments[i].segment.end.y, translateAfter.segments[i].segment.end.y, 1e-4f);
        }
        ASSERT_EQ(rotateBefore.segments.size(), rotateAfter.segments.size());
        EXPECT_NEAR(rotateBefore.segments.front().segment.start.x, rotateAfter.segments.front().segment.start.x, 1e-4f);

        const auto axisStemEnd = [](const Editor::ProjectedGizmo& gizmo, Editor::GizmoAxis axis) {
            for (const auto& item : gizmo.segments)
                if (item.axis == axis) return item.segment.end;
            return glm::vec2{};
        };
        const glm::vec2 beforeYEnd = axisStemEnd(scaleBefore, Editor::GizmoAxis::Y), afterXEnd = axisStemEnd(scaleAfter, Editor::GizmoAxis::X);
        EXPECT_NEAR(afterXEnd.x, beforeYEnd.x, 1e-2f);
        EXPECT_NEAR(afterXEnd.y, beforeYEnd.y, 1e-2f);

        const glm::vec2 xTip = axisStemEnd(translateBefore, Editor::GizmoAxis::X);
        const glm::vec2 yMid = (translateBefore.segments[3].segment.start + axisStemEnd(translateBefore, Editor::GizmoAxis::Y)) * 0.5f;
        EXPECT_EQ(Editor::pickGizmoAxis(xTip, translateBefore, 8.0f), Editor::GizmoAxis::X);
        EXPECT_EQ(Editor::pickGizmoAxis(yMid + glm::vec2(7.5f, 0.0f), translateBefore, 8.0f), Editor::GizmoAxis::Y);
        EXPECT_EQ(Editor::pickGizmoAxis(yMid + glm::vec2(8.5f, 0.0f), translateBefore, 8.0f), Editor::GizmoAxis::None);
        EXPECT_EQ(Editor::pickGizmoAxis(translateBefore.segments.front().segment.start, translateBefore, 8.0f), Editor::GizmoAxis::X);

        Editor::ProjectedGizmo synthetic; synthetic.visible = true;
        synthetic.segments = {{Editor::GizmoAxis::Z, {{100, 100}, {150, 100}}}, {Editor::GizmoAxis::Y, {{100, 100}, {100, 150}}}};
        EXPECT_EQ(Editor::pickGizmoAxis({100.0f, 100.0f}, synthetic, 8.0f), Editor::GizmoAxis::Y);
        EXPECT_EQ(Editor::pickGizmoAxis({130.0f, 103.0f}, synthetic, 8.0f), Editor::GizmoAxis::Z);

        glm::vec2 zRingPoint{};
        int zCount = 0;
        for (const auto& item : rotateBefore.segments)
            if (item.axis == Editor::GizmoAxis::Z && zCount++ == 6) { zRingPoint = (item.segment.start + item.segment.end) * 0.5f; break; }
        EXPECT_EQ(Editor::pickGizmoAxis(zRingPoint, rotateBefore, 8.0f), Editor::GizmoAxis::Z);
        EXPECT_EQ(Editor::pickGizmoAxis({std::numeric_limits<float>::quiet_NaN(), 0.0f}, rotateBefore, 8.0f), Editor::GizmoAxis::None);
    }

    TEST(EditorGizmoTest, TranslateRotateAndScaleSolversWithDegenerateGuards)
    {
        const Render::FrameExtent extent{800, 600};
        const App::ViewportRegion region{{100.0, 50.0}, {800.0, 600.0}};
        const auto isFiniteTransform = [](const Scene::Transform& t) {
            return std::isfinite(t.position().x) && std::isfinite(t.position().y) && std::isfinite(t.position().z) &&
                   std::isfinite(t.rotation().w) && std::isfinite(t.rotation().x) && std::isfinite(t.rotation().y) &&
                   std::isfinite(t.rotation().z) && std::isfinite(t.scale().x) && std::isfinite(t.scale().y) &&
                   std::isfinite(t.scale().z);
        };
        const auto axisTip = [](const Editor::ProjectedGizmo& gizmo, Editor::GizmoAxis axis) -> glm::vec2 {
            for (const auto& seg : gizmo.segments)
                if (seg.axis == axis)
                    return seg.segment.end;
            return {};
        };

        // 1. Translate：三轴独立平移、跨出视口外推及视线平行退化保护。
        {
            Scene::Scene scene;
            scene.setCamera(Scene::Camera({2.5f, 2.0f, 6.0f}));
            const auto id = scene.createEntity(
                nullptr, Scene::Transform({0.0f, 0.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {1.0f, 2.0f, 3.0f}),
                true, "GizmoTarget");
            const auto* entity = scene.find(id);
            ASSERT_NE(entity, nullptr);
            const auto gizmo = Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, region);
            ASSERT_TRUE(gizmo.visible);

            for (const auto [axis, comp] : std::array<std::pair<Editor::GizmoAxis, int>, 3>{
                     {{Editor::GizmoAxis::X, 0}, {Editor::GizmoAxis::Y, 1}, {Editor::GizmoAxis::Z, 2}}})
            {
                const glm::vec2 startPos = axisTip(gizmo, axis);
                const glm::vec2 endPos = startPos + (startPos - gizmo.segments.front().segment.start);
                auto drag = Editor::beginGizmoDrag(id, Editor::GizmoMode::Translate, axis, entity->transform,
                                                   scene.camera(), extent, region, startPos);
                ASSERT_TRUE(drag.active);
                const auto updated = Editor::updateGizmoDrag(drag, scene.camera(), extent, region, endPos);
                EXPECT_TRUE(isFiniteTransform(updated));
                EXPECT_GT(updated.position()[comp], 0.1f);
                EXPECT_NEAR(updated.position()[(comp + 1) % 3], 0.0f, 1e-4f);
                EXPECT_NEAR(updated.position()[(comp + 2) % 3], 0.0f, 1e-4f);
                EXPECT_EQ(updated.rotation(), entity->transform.rotation());
                EXPECT_EQ(updated.scale(), entity->transform.scale());
            }

            const glm::vec2 xStart = axisTip(gizmo, Editor::GizmoAxis::X);
            auto outDrag = Editor::beginGizmoDrag(id, Editor::GizmoMode::Translate, Editor::GizmoAxis::X,
                                                  entity->transform, scene.camera(), extent, region, xStart);
            ASSERT_TRUE(outDrag.active);
            const auto outUpdated =
                Editor::updateGizmoDrag(outDrag, scene.camera(), extent, region, glm::vec2(1200.0f, xStart.y));
            EXPECT_TRUE(isFiniteTransform(outUpdated));
            EXPECT_GT(outUpdated.position().x, 1.0f);

            Scene::Camera parallelCam({0.0f, 0.0f, 6.0f});
            const glm::vec2 centerCursor(region.origin.x + region.size.x * 0.5,
                                         region.origin.y + region.size.y * 0.5);
            auto parallelDrag =
                Editor::beginGizmoDrag(id, Editor::GizmoMode::Translate, Editor::GizmoAxis::Z, entity->transform,
                                       parallelCam, extent, region, centerCursor);
            ASSERT_TRUE(parallelDrag.active);
            const auto parallelResult =
                Editor::updateGizmoDrag(parallelDrag, parallelCam, extent, region, centerCursor + glm::vec2(5.0f, 0.0f));
            EXPECT_TRUE(isFiniteTransform(parallelResult));
            EXPECT_EQ(parallelResult.position(), entity->transform.position());
        }

        // 2. Rotate：平面环转角与掠射切线锁定不跳变。
        {
            Scene::Scene scene;
            scene.setCamera(Scene::Camera({0.0f, 0.0f, 6.0f}));
            const auto id = scene.createEntity(nullptr, Scene::Transform{}, true, "RotateTarget");
            const auto* entity = scene.find(id);
            ASSERT_NE(entity, nullptr);
            const auto gizmo = Editor::projectGizmo(scene, id, Editor::GizmoMode::Rotate, extent, region);
            ASSERT_TRUE(gizmo.visible);

            const glm::dmat4 vp =
                glm::dmat4(scene.camera().matrices(extent).projection) * glm::dmat4(scene.camera().matrices(extent).view);
            auto projectWorldPoint = [&](const glm::dvec3& p) -> glm::vec2 {
                const glm::dvec4 c = vp * glm::dvec4(p, 1.0);
                return {region.origin.x + (c.x / c.w + 1.0) * 0.5 * region.size.x,
                        region.origin.y + (c.y / c.w + 1.0) * 0.5 * region.size.y};
            };
            auto planarDrag = Editor::beginGizmoDrag(id, Editor::GizmoMode::Rotate, Editor::GizmoAxis::Z,
                                                     entity->transform, scene.camera(), extent, region,
                                                     projectWorldPoint({gizmo.worldLength, 0.0, 0.0}));
            ASSERT_TRUE(planarDrag.active && planarDrag.planarMode);
            const auto rotated90 = Editor::updateGizmoDrag(planarDrag, scene.camera(), extent, region,
                                                           projectWorldPoint({0.0, gizmo.worldLength, 0.0}));
            EXPECT_TRUE(isFiniteTransform(rotated90));
            const glm::vec3 xAfter = rotated90.rotation() * glm::vec3(1.0f, 0.0f, 0.0f);
            EXPECT_NEAR(xAfter.x, 0.0f, 1e-2f);
            EXPECT_NEAR(xAfter.y, 1.0f, 1e-2f);
            EXPECT_NEAR(xAfter.z, 0.0f, 1e-2f);

            const glm::vec2 ringCenter = projectWorldPoint({0.0, 0.0, 0.0});
            auto grazingDrag = Editor::beginGizmoDrag(id, Editor::GizmoMode::Rotate, Editor::GizmoAxis::Y,
                                                      entity->transform, scene.camera(), extent, region, ringCenter);
            ASSERT_TRUE(grazingDrag.active && !grazingDrag.planarMode);
            const glm::vec2 tangentPx(grazingDrag.startScreenTangent);
            const auto step1 = Editor::updateGizmoDrag(grazingDrag, scene.camera(), extent, region,
                                                       ringCenter + tangentPx * 45.0f + glm::vec2(0.0f, 200.0f));
            const auto step2 = Editor::updateGizmoDrag(grazingDrag, scene.camera(), extent, region,
                                                       ringCenter + tangentPx * 46.0f + glm::vec2(0.0f, 200.0f));
            EXPECT_TRUE(isFiniteTransform(step1) && isFiniteTransform(step2) && !grazingDrag.planarMode);
            const glm::vec3 z1 = step1.rotation() * glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 z2 = step2.rotation() * glm::vec3(0.0f, 0.0f, 1.0f);
            EXPECT_GT(z1.x, 0.2f);
            EXPECT_NEAR(glm::length(z2 - z1), 1.0f / 90.0f, 0.02f);
        }

        // 3. Scale：旋转局部轴缩放与零初始分量加性恢复。
        {
            Scene::Scene scene;
            scene.setCamera(Scene::Camera({2.0f, 2.0f, 6.0f}));
            const auto id = scene.createEntity(
                nullptr,
                Scene::Transform({0.0f, 0.0f, 0.0f}, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)),
                                 {0.0f, 2.0f, 1.0f}),
                true, "ScaleTarget");
            const auto* entity = scene.find(id);
            ASSERT_NE(entity, nullptr);
            const auto gizmo = Editor::projectGizmo(scene, id, Editor::GizmoMode::Scale, extent, region);
            ASSERT_TRUE(gizmo.visible);

            auto zeroScaleDrag = Editor::beginGizmoDrag(id, Editor::GizmoMode::Scale, Editor::GizmoAxis::X,
                                                        entity->transform, scene.camera(), extent, region,
                                                        gizmo.segments[0].segment.start);
            ASSERT_TRUE(zeroScaleDrag.active);
            const auto zeroRecovered = Editor::updateGizmoDrag(zeroScaleDrag, scene.camera(), extent, region,
                                                               gizmo.segments[0].segment.end);
            EXPECT_TRUE(isFiniteTransform(zeroRecovered));
            EXPECT_GT(zeroRecovered.scale().x, 0.5f);
            EXPECT_FLOAT_EQ(zeroRecovered.scale().y, 2.0f);
            EXPECT_FLOAT_EQ(zeroRecovered.scale().z, 1.0f);
            EXPECT_EQ(zeroRecovered.position(), entity->transform.position());
            EXPECT_EQ(zeroRecovered.rotation(), entity->transform.rotation());
        }
    }

    TEST(ViewportDebugLinesCpuTest, GeneratesGridAndLightGizmoSegmentsWithoutImGui)
    {
        Scene::Scene scene;
        Editor::ViewportDebugOptions opts{false, false, false};
        EXPECT_TRUE(Editor::buildViewportDebugLines(scene, {}, opts).empty());

        opts.showGrid = true;
        const auto gridLines = Editor::buildViewportDebugLines(scene, {}, opts);
        ASSERT_EQ(gridLines.size(), 42u);
        EXPECT_NEAR(gridLines[20].start.z, 0.0f, 1e-5f);
        EXPECT_GT(gridLines[20].color.r, gridLines[20].color.b);  // X 主轴为红色
        EXPECT_GT(gridLines[21].color.b, gridLines[21].color.r);  // Z 主轴为蓝色

        opts.showLightGizmos = true;
        const auto withDirOnly = Editor::buildViewportDebugLines(scene, {}, opts);
        EXPECT_EQ(withDirOnly.size(), 42u + 3u);

        Render::Lighting custom;
        Render::LocalLight point;
        point.type = Render::LocalLightType::Point;
        point.position = {2.0f, 3.0f, -1.0f};
        point.color = {0.8f, 0.2f, 0.1f};
        Render::LocalLight spot;
        spot.type = Render::LocalLightType::Spot;
        spot.position = {-2.0f, 4.0f, 1.0f};
        spot.direction = {0.0f, -1.0f, 0.0f};
        spot.range = 2.0f;
        spot.outerConeAngle = 0.5f;
        Render::LocalLight disabledPoint = point;
        disabledPoint.enabled = false;
        custom.localLights = {point, spot, disabledPoint};
        scene.setLighting(custom);

        const auto allLines = Editor::buildViewportDebugLines(scene, {}, opts);
        // 42 网格 + 3 方向光 + 36 点光 + 21 聚光 + 36 禁用点光 = 138
        EXPECT_EQ(allLines.size(), 138u);
        // 第一个点光环顶点距离点光源中心应恰好等于 0.35f。
        EXPECT_NEAR(glm::distance(allLines[45].start, point.position), 0.35f, 1e-4f);
        // 禁用点光使用低亮度灰标记。
        EXPECT_FLOAT_EQ(allLines.back().alpha, 0.45f);
    }
}  // namespace
