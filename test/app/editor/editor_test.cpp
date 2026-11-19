// 编译期确认操作接口与 Gizmo 头文件独立于 ImGui。
#include "app/editor/editor_operations.h"
#include "app/editor/gizmo.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glm/ext/matrix_transform.hpp>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "app/common/lighting_controls.h"
#include "app/common/startup_config.h"
#include "app/editor/editor_app.h"
#include "app/editor/editor_scene.h"
#include "app/editor/selection_overlay.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/model/prepared_model.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/resource/loader/text_resource.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
#include "runtime/scene/scene_file.h"
#include "runtime/scene/scene_frame.h"
#include "support/texture_gpu_review.h"
#include "support/window_capture.h"
#include "test/app/editor/showcase_scene.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    namespace fs = std::filesystem;
    namespace NativeIO = Incubator::FileIO;

    TEST(EditorSelectionTest, EditsByIdAndDoesNotRetainScenePointers)
    {
        Editor::EditorSelection selection;
        Scene::Scene scene;
        const auto first = scene.createEntity();
        const auto second = scene.createEntity();
        selection.select(first);
        EXPECT_TRUE(selection.rename(scene, "中文 ## 名称"));
        EXPECT_TRUE(selection.setVisible(scene, false));
        EXPECT_TRUE(selection.setTransform(scene, Scene::Transform({1, 2, 3})));
        EXPECT_EQ(scene.find(first)->name, "中文 ## 名称");
        EXPECT_FALSE(scene.find(first)->visible);
        EXPECT_EQ(scene.find(first)->transform.position(), glm::vec3(1, 2, 3));
        EXPECT_TRUE(scene.find(second)->name.empty());
        selection.remove(scene);
        EXPECT_FALSE(selection.selected().valid());
        EXPECT_EQ(scene.find(first), nullptr);
        EXPECT_NE(scene.find(second), nullptr);
        selection.select(second);
        selection.clear(scene);
        EXPECT_FALSE(selection.selected().valid());
        EXPECT_TRUE(scene.entities().empty());
    }

    TEST(EditorSelectionTest, DuplicateNamesAndSelectionFollowEntityIdentity)
    {
        Editor::EditorSelection selection;
        Scene::Scene scene;
        const auto first = scene.createEntity({}, Scene::Transform({1, 2, 3}), false, "测试 ##");
        EXPECT_FALSE(selection.duplicate(scene).valid());
        selection.select(first);
        const auto copy = selection.duplicate(scene);
        ASSERT_TRUE(copy.valid());
        EXPECT_EQ(selection.selected(), copy);
        EXPECT_EQ(scene.find(first)->name, "测试 ##");
        EXPECT_EQ(scene.find(copy)->name, "测试 ## 副本");
        EXPECT_FALSE(scene.find(copy)->visible);
        EXPECT_EQ(scene.find(copy)->transform.matrix(), scene.find(first)->transform.matrix());
        const auto empty = scene.createEntity();
        selection.select(empty);
        const auto unnamed = selection.duplicate(scene);
        ASSERT_TRUE(unnamed.valid());
        EXPECT_EQ(scene.find(unnamed)->name, "未命名 副本");
        ASSERT_TRUE(scene.destroyEntity(unnamed));
        EXPECT_FALSE(selection.duplicate(scene).valid());
        selection.remove(scene);
        EXPECT_FALSE(selection.selected().valid());
        EXPECT_EQ(scene.entities().size(), 3u);
    }

    TEST(EditorOperationsDirtyTest, CompositionTracksPersistentChangesAndIgnoresNoOps)
    {
        static_assert(!std::is_base_of_v<Editor::EditorSelection, Editor::EditorOperations>,
                      "EditorOperations 应组合而非公有继承 EditorSelection");

        Editor::EditorOperations operations;
        Scene::Scene scene;
        std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1.0f};
        EXPECT_FALSE(operations.isDirty());

        // 空场景清理、无效选中删除/复制不置脏。
        EXPECT_FALSE(operations.clear(scene, clearColor));
        EXPECT_FALSE(operations.remove(scene, clearColor));
        EXPECT_FALSE(operations.duplicate(scene, clearColor).valid());
        EXPECT_FALSE(operations.isDirty());

        const auto first = scene.createEntity({}, Scene::Transform({1, 0, 0}), true, "对象A");
        const auto second = scene.createEntity({}, Scene::Transform({2, 0, 0}), true, "对象B");

        // 仅切换选择不置脏。
        operations.select(first);
        EXPECT_EQ(operations.selected(), first);
        EXPECT_FALSE(operations.isDirty());

        // 设置与当前值相同的数据不置脏。
        EXPECT_TRUE(operations.rename(scene, clearColor, "对象A"));
        EXPECT_TRUE(operations.setVisible(scene, clearColor, true));
        EXPECT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({1, 0, 0})));
        EXPECT_TRUE(operations.setLighting(scene, clearColor, scene.lighting()));
        EXPECT_TRUE(operations.setClearColor(scene, clearColor, clearColor));
        EXPECT_FALSE(operations.isDirty());

        // 真实修改（名称、可见性、TRS、灯光、背景色、复制、删除、清空）均正确置脏。
        const auto checkDirty = [&](auto fn) {
            Editor::EditorOperations op;
            Scene::Scene s;
            std::array<float, 4> bg{0.1f, 0.2f, 0.3f, 1.0f};
            const auto id = s.createEntity({}, Scene::Transform({1, 0, 0}), true, "初始");
            op.select(id);
            EXPECT_FALSE(op.isDirty());
            fn(op, s, bg, id);
            EXPECT_TRUE(op.isDirty());
        };

        checkDirty([](auto& op, auto& s, const auto& bg, auto) { EXPECT_TRUE(op.rename(s, bg, "新名称")); });
        checkDirty([](auto& op, auto& s, const auto& bg, auto) { EXPECT_TRUE(op.setVisible(s, bg, false)); });
        checkDirty([](auto& op, auto& s, const auto& bg, auto) {
            EXPECT_TRUE(op.setTransform(s, bg, Scene::Transform({3, 4, 5})));
        });
        checkDirty([](auto& op, auto& s, const auto& bg, auto) {
            EXPECT_TRUE(op.duplicate(s, bg).valid());
        });
        checkDirty([](auto& op, auto& s, const auto& bg, auto) { EXPECT_TRUE(op.remove(s, bg)); });
        checkDirty([](auto& op, auto& s, const auto& bg, auto) { EXPECT_TRUE(op.clear(s, bg)); });
        checkDirty([](auto& op, auto& s, const auto& bg, auto) {
            auto l = s.lighting();
            l.intensity = 2.5f;
            EXPECT_TRUE(op.setLighting(s, bg, l));
        });
        checkDirty([](auto& op, const auto& s, auto& bg, auto) {
            EXPECT_TRUE(op.setClearColor(s, bg, {0.5f, 0.5f, 0.5f, 1.0f}));
        });

        // 删除场景外部已被销毁的实体不置脏。
        EXPECT_TRUE(scene.destroyEntity(second));
        operations.select(second);
        EXPECT_FALSE(operations.remove(scene, clearColor));
        EXPECT_FALSE(operations.isDirty());
    }

    TEST(EditorOperationsHistoryTest, DiscreteOperationsUndoRedoCapacityAndRevisionDirtySync)
    {
        Editor::EditorOperations operations;
        Scene::Scene scene;
        std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1.0f};

        const auto idA = scene.createEntity({}, Scene::Transform({1, 0, 0}), true, "原始A");
        const auto idB = scene.createEntity({}, Scene::Transform({2, 0, 0}), false, "原始B");
        EXPECT_FALSE(operations.canUndo());
        EXPECT_FALSE(operations.canRedo());
        EXPECT_FALSE(operations.isDirty());

        // 1. 改名 -> 2. 可见性 -> 3. 变换 -> 4. 复制 -> 5. 灯光 -> 6. 背景色 -> 7. 删除 -> 8. 清空。
        operations.select(idA);
        ASSERT_TRUE(operations.rename(scene, clearColor, "重命名A"));
        ASSERT_TRUE(operations.setVisible(scene, clearColor, false));
        ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({9, 8, 7})));
        const auto idCopy = operations.duplicate(scene, clearColor);
        ASSERT_TRUE(idCopy.valid());
        auto lighting = scene.lighting();
        lighting.intensity = 2.0f;
        ASSERT_TRUE(operations.setLighting(scene, clearColor, lighting));
        ASSERT_TRUE(operations.setClearColor(scene, clearColor, {0.4f, 0.5f, 0.6f, 1.0f}));
        // 异常或无变化操作不污染历史栈。
        EXPECT_THROW(operations.rename(scene, clearColor, std::string("\xff", 1)), Error::Exception);
        auto badLighting = scene.lighting();
        badLighting.direction = {0, 0, 0};
        EXPECT_THROW(operations.setLighting(scene, clearColor, badLighting), Error::Exception);
        operations.select(idA);
        ASSERT_TRUE(operations.remove(scene, clearColor));
        EXPECT_EQ(scene.find(idA), nullptr);
        ASSERT_TRUE(operations.clear(scene, clearColor));
        EXPECT_TRUE(scene.entities().empty());
        EXPECT_TRUE(operations.isDirty());

        // 清空后无效选中返回 false，不抛出也不入栈。
        operations.select(idCopy);
        EXPECT_FALSE(operations.rename(scene, clearColor, "missing"));

        // 撤销清空 -> 撤销删除（恢复原始 idA 与顺序）-> 撤销背景色 -> 撤销灯光 -> 撤销复制 -> 撤销变换 -> 撤销可见性 -> 撤销改名。
        ASSERT_TRUE(operations.undo(scene, clearColor));
        ASSERT_EQ(scene.entities().size(), 2u);
        EXPECT_EQ(scene.entities()[0].id, idB);
        EXPECT_EQ(scene.entities()[1].id, idCopy);

        ASSERT_TRUE(operations.undo(scene, clearColor));
        ASSERT_EQ(scene.entities().size(), 3u);
        EXPECT_EQ(scene.entities()[0].id, idA);
        EXPECT_EQ(scene.entities()[0].name, "重命名A");
        EXPECT_FALSE(scene.entities()[0].visible);
        EXPECT_EQ(scene.entities()[0].transform.position(), glm::vec3(9, 8, 7));
        EXPECT_EQ(operations.selected(), idA);

        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_EQ(clearColor, (std::array<float, 4>{0.12f, 0.16f, 0.22f, 1.0f}));
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_FLOAT_EQ(scene.lighting().intensity, 1.0f);
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_EQ(scene.find(idCopy), nullptr);
        EXPECT_EQ(operations.selected(), idA);

        // 撤销先前对同一 idA 的变换、可见性和改名。
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_EQ(scene.find(idA)->transform.position(), glm::vec3(1, 0, 0));
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_TRUE(scene.find(idA)->visible);
        EXPECT_TRUE(operations.isDirty());
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_EQ(scene.find(idA)->name, "原始A");
        // 恰好回到初始修订号：isDirty 自动恢复 false。
        EXPECT_FALSE(operations.isDirty());
        EXPECT_FALSE(operations.canUndo());
        EXPECT_TRUE(operations.canRedo());

        // 之后新建实体的 ID 不与曾存在的 idCopy 冲突。
        const auto idNew = scene.createEntity();
        EXPECT_GT(idNew.value(), idCopy.value());
        EXPECT_TRUE(scene.destroyEntity(idNew));

        // 连续重做前 3 步并检查状态与脏标记，再在中间分支做新编辑截断 redoStack_。
        ASSERT_TRUE(operations.redo(scene, clearColor));
        EXPECT_TRUE(operations.isDirty());
        EXPECT_EQ(scene.find(idA)->name, "重命名A");
        ASSERT_TRUE(operations.redo(scene, clearColor));
        EXPECT_FALSE(scene.find(idA)->visible);
        EXPECT_TRUE(operations.canRedo());

        ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({5, 5, 5})));
        EXPECT_FALSE(operations.canRedo());

        // 测试 64 步容量上限：执行 65 次有效修改，最多只能 undo 64 次，且第 0 步基线被淘汰后 isDirty 保持 true。
        Editor::EditorOperations bounded;
        Scene::Scene boundedScene;
        std::array<float, 4> boundedColor{0, 0, 0, 1};
        const auto target = boundedScene.createEntity({}, Scene::Transform({0, 0, 0}));
        bounded.select(target);
        for (int step = 1; step <= 65; ++step)
            ASSERT_TRUE(bounded.setTransform(boundedScene, boundedColor, Scene::Transform({float(step), 0, 0})));

        size_t undos = 0;
        while (bounded.undo(boundedScene, boundedColor))
            ++undos;
        EXPECT_EQ(undos, Editor::EditorOperations::MAX_HISTORY);
        EXPECT_EQ(boundedScene.find(target)->transform.position().x, 1.0f);
        EXPECT_TRUE(bounded.isDirty());
    }

    TEST(EditorOperationsHistoryTest, TransactionsMergeNoOpDiscardAndCancelRollback)
    {
        Editor::EditorOperations operations;
        Scene::Scene scene;
        std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1.0f};
        const auto entityId = scene.createEntity({}, Scene::Transform({0, 0, 0}), true, "初始名称");
        operations.select(entityId);

        EXPECT_FALSE(operations.inTransaction());
        EXPECT_FALSE(operations.commitTransaction(scene, clearColor));
        EXPECT_FALSE(operations.cancelTransaction(scene, clearColor));

        // 1. 事务中的多次连续属性编辑折叠为 1 条 undo，过程中不压栈。
        operations.beginTransaction(scene, clearColor);
        EXPECT_TRUE(operations.inTransaction());
        ASSERT_TRUE(operations.rename(scene, clearColor, "阶段名称"));
        ASSERT_TRUE(operations.rename(scene, clearColor, "最终名称"));
        ASSERT_TRUE(operations.setVisible(scene, clearColor, false));
        ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({1, 2, 3})));
        ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({4, 5, 6})));
        auto lighting = scene.lighting();
        lighting.intensity = 2.5f;
        ASSERT_TRUE(operations.setLighting(scene, clearColor, lighting));
        lighting.ambient = 0.45f;
        ASSERT_TRUE(operations.setLighting(scene, clearColor, lighting));
        ASSERT_TRUE(operations.setClearColor(scene, clearColor, {0.3f, 0.4f, 0.5f, 1.0f}));
        ASSERT_TRUE(operations.setClearColor(scene, clearColor, {0.6f, 0.7f, 0.8f, 1.0f}));
        EXPECT_FALSE(operations.canUndo());

        // 重复调用 beginTransaction 不会覆盖最初的事务起点。
        operations.beginTransaction(scene, clearColor);
        EXPECT_TRUE(operations.commitTransaction(scene, clearColor));
        EXPECT_FALSE(operations.inTransaction());
        EXPECT_TRUE(operations.canUndo());
        EXPECT_TRUE(operations.isDirty());

        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_FALSE(operations.canUndo());
        EXPECT_TRUE(operations.canRedo());
        EXPECT_FALSE(operations.isDirty());
        EXPECT_EQ(scene.find(entityId)->name, "初始名称");
        EXPECT_TRUE(scene.find(entityId)->visible);
        EXPECT_EQ(scene.find(entityId)->transform.position(), glm::vec3(0, 0, 0));
        EXPECT_FLOAT_EQ(scene.lighting().intensity, 1.0f);
        EXPECT_EQ(clearColor, (std::array<float, 4>{0.12f, 0.16f, 0.22f, 1.0f}));

        // 2. 无变化（含修改后再改回初始状态）提交时静默丢弃，不清空 redoStack_ 也不产生冗余历史。
        operations.beginTransaction(scene, clearColor);
        ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({9, 9, 9})));
        ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({0, 0, 0})));
        EXPECT_FALSE(operations.commitTransaction(scene, clearColor));
        EXPECT_FALSE(operations.canUndo());
        EXPECT_TRUE(operations.canRedo());
        EXPECT_FALSE(operations.isDirty());

        // 3. cancelTransaction 回滚回起始快照并保留现有历史栈与 redoStack_。
        operations.beginTransaction(scene, clearColor);
        ASSERT_TRUE(operations.rename(scene, clearColor, "被取消的修改"));
        ASSERT_TRUE(operations.setClearColor(scene, clearColor, {0.9f, 0.1f, 0.1f, 1.0f}));
        EXPECT_TRUE(operations.cancelTransaction(scene, clearColor));
        EXPECT_FALSE(operations.inTransaction());
        EXPECT_FALSE(operations.canUndo());
        EXPECT_TRUE(operations.canRedo());
        EXPECT_FALSE(operations.isDirty());
        EXPECT_EQ(scene.find(entityId)->name, "初始名称");
        EXPECT_EQ(clearColor, (std::array<float, 4>{0.12f, 0.16f, 0.22f, 1.0f}));

        // 重做刚才那条事务并检查离散动作会先结算挂起事务，再执行自身操作。
        ASSERT_TRUE(operations.redo(scene, clearColor));
        EXPECT_EQ(scene.find(entityId)->name, "最终名称");

        operations.beginTransaction(scene, clearColor);
        ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({10, 0, 0})));
        const auto copyId = operations.duplicate(scene, clearColor);
        ASSERT_TRUE(copyId.valid());
        EXPECT_FALSE(operations.inTransaction());
        // 第 1 次撤销先撤掉 duplicate，第 2 次撤销回到事务前位置 (4,5,6)。
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_EQ(scene.find(copyId), nullptr);
        EXPECT_EQ(scene.find(entityId)->transform.position(), glm::vec3(10, 0, 0));
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_EQ(scene.find(entityId)->transform.position(), glm::vec3(4, 5, 6));

        // undo / redo 在事务未提交时会先结算该事务后再执行撤销。
        operations.beginTransaction(scene, clearColor);
        auto modifiedLight = scene.lighting();
        modifiedLight.intensity = 3.5f;
        ASSERT_TRUE(operations.setLighting(scene, clearColor, modifiedLight));
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_FALSE(operations.inTransaction());
        EXPECT_FLOAT_EQ(scene.lighting().intensity, 2.5f);
    }

    TEST(EditorSelectionOverlayTest, ClipHomogeneousSegmentHandlesNearFarBehindCameraOffscreenAndNonFinite)
    {
        // 完全在视锥内部：保持原端点。
        const auto inside = Editor::clipHomogeneousSegment({-0.5, 0.25, 0.5, 2.0}, {0.5, -0.25, 1.5, 2.0});
        ASSERT_TRUE(inside.has_value());
        EXPECT_DOUBLE_EQ(inside->first.x, -0.5);
        EXPECT_DOUBLE_EQ(inside->second.z, 1.5);

        // 完全在相机后方（w < 0）或完全离屏：直接剔除。
        EXPECT_FALSE(Editor::clipHomogeneousSegment({0.0, 0.0, -1.0, -2.0}, {0.1, 0.1, -0.5, -1.0}).has_value());
        EXPECT_FALSE(Editor::clipHomogeneousSegment({3.0, 0.0, 0.5, 2.0}, {4.0, 0.0, 0.5, 2.0}).has_value());
        EXPECT_FALSE(Editor::clipHomogeneousSegment({0.0, 0.0, 2.5, 2.0}, {0.0, 0.0, 3.0, 2.0}).has_value());

        // 跨越近平面且起点位于相机后方（w < 0）：裁剪到 z = 0 且两端 w > 0。
        const auto acrossNear = Editor::clipHomogeneousSegment({0.0, 0.0, -1.0, -1.0}, {0.0, 0.0, 1.0, 3.0});
        ASSERT_TRUE(acrossNear.has_value());
        EXPECT_NEAR(acrossNear->first.z, 0.0, 1e-12);
        EXPECT_GT(acrossNear->first.w, 0.0);
        EXPECT_GT(acrossNear->second.w, 0.0);

        // 跨越远平面（z > w）：裁剪到 z = w。
        const auto acrossFar = Editor::clipHomogeneousSegment({0.0, 0.0, 0.5, 2.0}, {0.0, 0.0, 4.0, 2.0});
        ASSERT_TRUE(acrossFar.has_value());
        EXPECT_NEAR(acrossFar->second.z, acrossFar->second.w, 1e-12);

        // 跨越右侧裁剪面（x > w）：裁剪到 x = w。
        const auto acrossRight = Editor::clipHomogeneousSegment({0.0, 0.0, 0.5, 2.0}, {4.0, 0.0, 0.5, 2.0});
        ASSERT_TRUE(acrossRight.has_value());
        EXPECT_NEAR(acrossRight->second.x, acrossRight->second.w, 1e-12);

        // 非有限输入（NaN/Inf）安全剔除。
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        EXPECT_FALSE(Editor::clipHomogeneousSegment({nan, 0.0, 0.5, 1.0}, {0.0, 0.0, 0.5, 1.0}).has_value());
        EXPECT_FALSE(Editor::clipHomogeneousSegment({0.0, 0.0, 0.5, 1.0}, {inf, 0.0, 0.5, 1.0}).has_value());
    }

    TEST(EditorSelectionOverlayTest, ProjectAabbOverlayMatchesViewportMappingAcrossDpiAndClipsBounds)
    {
        const Scene::Camera camera({0.0f, 0.0f, 6.0f});
        const App::ViewportRegion region{{100.0, 50.0}, {800.0, 600.0}, true, true};
        const Render::FrameExtent extent{800, 600};
        const Math::Aabb box{{-1.0f, -0.5f, -1.0f}, {1.0f, 1.5f, 1.0f}};

        // 无效视口不产出线段。
        EXPECT_TRUE(Editor::projectAabbOverlay(box, camera, {0, 600}, region).empty());
        EXPECT_TRUE(Editor::projectAabbOverlay(box, camera, extent, {{100.0, 50.0}, {0.0, 600.0}, true, true}).empty());

        const auto segments = Editor::projectAabbOverlay(box, camera, extent, region);
        ASSERT_EQ(segments.size(), 12u);

        // 验证 Y 方向不重复翻转（世界 +Y 顶点应映射到屏幕更小的 y 坐标）且等比例 DPI 缩放不改变逻辑坐标。
        float minY = region.origin.y + region.size.y;
        float maxY = region.origin.y;
        for (const auto& s : segments)
        {
            for (const auto& p : {s.start, s.end})
            {
                EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
                EXPECT_GE(p.x, region.origin.x - 1e-3f);
                EXPECT_LE(p.x, region.origin.x + region.size.x + 1e-3f);
                EXPECT_GE(p.y, region.origin.y - 1e-3f);
                EXPECT_LE(p.y, region.origin.y + region.size.y + 1e-3f);
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
            }
        }
        const float centerY = static_cast<float>(region.origin.y + region.size.y * 0.5);
        // 盒子在 Y 上是 [-0.5, 1.5]（偏向上方 +Y），因此屏幕顶部距离中心应大于底部距离中心。
        EXPECT_GT(centerY - minY, maxY - centerY);

        for (float dpi : {1.5f, 2.0f})
        {
            const Render::FrameExtent scaled{static_cast<uint32_t>(extent.width * dpi),
                                             static_cast<uint32_t>(extent.height * dpi)};
            const auto scaledSegments = Editor::projectAabbOverlay(box, camera, scaled, region);
            ASSERT_EQ(scaledSegments.size(), segments.size());
            for (size_t i = 0; i < segments.size(); ++i)
            {
                EXPECT_NEAR(scaledSegments[i].start.x, segments[i].start.x, 1e-3f);
                EXPECT_NEAR(scaledSegments[i].start.y, segments[i].start.y, 1e-3f);
                EXPECT_NEAR(scaledSegments[i].end.x, segments[i].end.x, 1e-3f);
                EXPECT_NEAR(scaledSegments[i].end.y, segments[i].end.y, 1e-3f);
            }
        }

        // 跨越相机近平面与相机后方的包围盒：前 4 条边可见，4 条纵边在近平面处被裁剪，后 4 条边被剔除。
        const Math::Aabb acrossCamera{{-0.5f, -0.5f, 3.0f}, {0.5f, 0.5f, 9.0f}};
        const auto clippedSegments = Editor::projectAabbOverlay(acrossCamera, camera, extent, region);
        EXPECT_EQ(clippedSegments.size(), 8u);
        for (const auto& s : clippedSegments)
        {
            for (const auto& p : {s.start, s.end})
            {
                EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
                EXPECT_GE(p.x, region.origin.x - 1e-3f);
                EXPECT_LE(p.x, region.origin.x + region.size.x + 1e-3f);
                EXPECT_GE(p.y, region.origin.y - 1e-3f);
                EXPECT_LE(p.y, region.origin.y + region.size.y + 1e-3f);
            }
        }

        // 相机位于超大包围盒内部且 12 条边均在视锥外：全部正确剔除。
        const Math::Aabb enclosingCamera{{-2.0f, -2.0f, 3.0f}, {2.0f, 2.0f, 9.0f}};
        EXPECT_TRUE(Editor::projectAabbOverlay(enclosingCamera, camera, extent, region).empty());

        // 完全在相机后方或完全离屏的包围盒：返回空。
        const Math::Aabb behindCamera{{-1.0f, -1.0f, 7.0f}, {1.0f, 1.0f, 10.0f}};
        EXPECT_TRUE(Editor::projectAabbOverlay(behindCamera, camera, extent, region).empty());
        const Math::Aabb offscreen{{50.0f, 0.0f, -2.0f}, {52.0f, 2.0f, 0.0f}};
        EXPECT_TRUE(Editor::projectAabbOverlay(offscreen, camera, extent, region).empty());

        // 无选择/无模型实体不产出线段。
        Scene::Scene emptyScene;
        const auto noModel = emptyScene.createEntity();
        EXPECT_TRUE(Editor::projectSelectionBox(emptyScene, {}, extent, region).empty());
        EXPECT_TRUE(Editor::projectSelectionBox(emptyScene, noModel, extent, region).empty());
    }

    TEST(EditorGizmoTest, ViewDepthConstantScreenScaleAndViewportAlignment)
    {
        Scene::Scene scene;
        auto camera = scene.camera();
        camera.setPosition({0.0f, 0.0f, 5.0f});
        camera.setRotation({1.0f, 0.0f, 0.0f, 0.0f});
        Scene::Perspective persp;
        persp.verticalFov = glm::radians(60.0f);
        persp.nearPlane = 0.1f;
        persp.farPlane = 100.0f;
        camera.setPerspective(persp);
        scene.setCamera(camera);

        // 无模型但可见的实体同样允许生成 Gizmo 图元。
        const auto entityId = scene.createEntity(nullptr, Scene::Transform{}, true, "EmptyPivot");
        const Render::FrameExtent extent{800, 600};
        const App::ViewportRegion region{{100.0, 50.0}, {800.0, 600.0}, true, true};

        auto yAxisScreenLength = [](const Editor::ProjectedGizmo& gizmo) {
            for (const auto& item : gizmo.segments)
            {
                if (item.axis == Editor::GizmoAxis::Y)
                    return glm::length(item.segment.end - item.segment.start);
            }
            return 0.0f;
        };

        const auto centerGizmo =
            Editor::projectGizmo(scene, entityId, Editor::GizmoMode::Translate, extent, region, 90.0f);
        ASSERT_TRUE(centerGizmo.visible);
        ASSERT_EQ(centerGizmo.segments.size(), 9u);
        EXPECT_NEAR(yAxisScreenLength(centerGizmo), 90.0f, 1e-2f);

        // 对齐校验：对称于原点的世界 AABB 中心应与 Gizmo 原点屏幕坐标严格一致（Y 轴仅翻转一次）。
        const auto aabbSegs =
            Editor::projectAabbOverlay({{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}}, camera, extent, region);
        ASSERT_EQ(aabbSegs.size(), 12u);
        glm::vec2 aabbCenter(0.0f);
        for (const auto& s : aabbSegs)
            aabbCenter += s.start + s.end;
        aabbCenter /= static_cast<float>(aabbSegs.size() * 2);
        EXPECT_NEAR(centerGizmo.segments.front().segment.start.x, aabbCenter.x, 1e-3f);
        EXPECT_NEAR(centerGizmo.segments.front().segment.start.y, aabbCenter.y, 1e-3f);

        // 同一视图深度下移动到靠近视口右边缘：必须基于 z_view 而非欧氏距离，屏幕 Y 轴长度仍恒定。
        ASSERT_TRUE(scene.setTransform(entityId, Scene::Transform({2.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3(1.0f))));
        const auto edgeGizmo =
            Editor::projectGizmo(scene, entityId, Editor::GizmoMode::Translate, extent, region, 90.0f);
        ASSERT_TRUE(edgeGizmo.visible);
        EXPECT_NEAR(edgeGizmo.worldLength, centerGizmo.worldLength, 1e-4f);
        EXPECT_NEAR(yAxisScreenLength(edgeGizmo), 90.0f, 1e-2f);

        // 相机拉远后世界轴长线性增大，屏幕逻辑长度仍保持 90 px。
        ASSERT_TRUE(scene.setTransform(entityId, Scene::Transform{}));
        camera.setPosition({0.0f, 0.0f, 15.0f});
        scene.setCamera(camera);
        const auto farGizmo = Editor::projectGizmo(scene, entityId, Editor::GizmoMode::Translate, extent, region, 90.0f);
        ASSERT_TRUE(farGizmo.visible);
        EXPECT_NEAR(farGizmo.worldLength, centerGizmo.worldLength * 3.0f, 1e-3f);
        EXPECT_NEAR(yAxisScreenLength(farGizmo), 90.0f, 1e-2f);

        // 改变宽高比与 2x DPI 渲染尺寸但保持逻辑视口大小：屏幕逻辑尺度与投影中心依然保持一致。
        const Render::FrameExtent wideDpiExtent{2400, 1200};
        const App::ViewportRegion wideRegion{{100.0, 50.0}, {1200.0, 600.0}, true, true};
        const auto wideGizmo =
            Editor::projectGizmo(scene, entityId, Editor::GizmoMode::Translate, wideDpiExtent, wideRegion, 90.0f);
        ASSERT_TRUE(wideGizmo.visible);
        EXPECT_NEAR(yAxisScreenLength(wideGizmo), 90.0f, 1e-2f);
        EXPECT_NEAR(wideGizmo.segments.front().segment.start.x, 700.0f, 1e-2f);
        EXPECT_NEAR(wideGizmo.segments.front().segment.start.y, 350.0f, 1e-2f);
    }

    TEST(EditorGizmoTest, ClippingBehindNearPlaneAndHiddenEntitySafety)
    {
        Scene::Scene scene;
        auto camera = scene.camera();
        camera.setPosition({0.0f, 0.0f, 5.0f});
        camera.setRotation({1.0f, 0.0f, 0.0f, 0.0f});
        Scene::Perspective persp;
        persp.verticalFov = glm::radians(60.0f);
        persp.nearPlane = 0.5f;
        persp.farPlane = 50.0f;
        camera.setPerspective(persp);
        scene.setCamera(camera);

        const Render::FrameExtent extent{800, 600};
        const App::ViewportRegion region{{20.0, 10.0}, {800.0, 600.0}, true, true};
        const auto id = scene.createEntity(nullptr, Scene::Transform{}, true, "Target");

        // 无效选中、无效视口、隐藏实体均返回空。
        EXPECT_FALSE(Editor::projectGizmo(scene, {}, Editor::GizmoMode::Translate, extent, region).visible);
        EXPECT_FALSE(Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, {0, 600}, region).visible);
        EXPECT_FALSE(
            Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, {{0.0, 0.0}, {0.0, 100.0}}).visible);

        ASSERT_TRUE(scene.setVisible(id, false));
        EXPECT_FALSE(Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, region).visible);
        ASSERT_TRUE(scene.setVisible(id, true));

        // 中心位于近平面后方（zView < nearPlane）与相机背后：返回空结果。
        ASSERT_TRUE(scene.setTransform(id, Scene::Transform({0.0f, 0.0f, 4.7f}, {1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3(1.0f))));
        const auto behindNear = Editor::projectGizmo(scene, id, Editor::GizmoMode::Rotate, extent, region);
        EXPECT_FALSE(behindNear.visible);
        EXPECT_TRUE(behindNear.segments.empty());

        ASSERT_TRUE(scene.setTransform(id, Scene::Transform({0.0f, 0.0f, 8.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3(1.0f))));
        EXPECT_FALSE(Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, region).visible);

        // 中心恰好在近平面前一点点，旋转圆环跨越近平面与视口边缘：裁剪端点必须全部有限且处于视口内。
        ASSERT_TRUE(scene.setTransform(id, Scene::Transform({0.0f, 0.0f, 4.45f}, {1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3(1.0f))));
        const auto acrossNear = Editor::projectGizmo(scene, id, Editor::GizmoMode::Rotate, extent, region, 300.0f);
        ASSERT_TRUE(acrossNear.visible);
        ASSERT_FALSE(acrossNear.segments.empty());
        for (const auto& item : acrossNear.segments)
        {
            for (const auto& p : {item.segment.start, item.segment.end})
            {
                EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
                EXPECT_GE(p.x, region.origin.x - 1e-2f);
                EXPECT_LE(p.x, region.origin.x + region.size.x + 1e-2f);
                EXPECT_GE(p.y, region.origin.y - 1e-2f);
                EXPECT_LE(p.y, region.origin.y + region.size.y + 1e-2f);
            }
        }
    }

    TEST(EditorGizmoTest, ModeAxesOrientationAndPickGizmoAxisPriority)
    {
        Scene::Scene scene;
        auto camera = scene.camera();
        camera.setPosition({0.0f, 0.0f, 5.0f});
        camera.setRotation({1.0f, 0.0f, 0.0f, 0.0f});
        scene.setCamera(camera);

        const Render::FrameExtent extent{800, 600};
        const App::ViewportRegion region{{0.0, 0.0}, {800.0, 600.0}, true, true};
        const auto id = scene.createEntity(nullptr, Scene::Transform{}, true, "OrientationTarget");

        // 旋转前后的 Translate 与 Rotate 均保持世界轴向。
        const auto translateBefore = Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, region);
        const auto rotateBefore = Editor::projectGizmo(scene, id, Editor::GizmoMode::Rotate, extent, region);
        const auto scaleBefore = Editor::projectGizmo(scene, id, Editor::GizmoMode::Scale, extent, region);

        ASSERT_TRUE(scene.setTransform(
            id, Scene::Transform({0.0f, 0.0f, 0.0f},
                                 glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)), glm::vec3(1.0f))));
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

        // Scale 模式下实体绕 +Z 旋转 90 度后，局部 +X 轴与未旋转时的 +Y 轴指向一致（屏幕上方）。
        auto findAxisStemEnd = [](const Editor::ProjectedGizmo& gizmo, Editor::GizmoAxis axis) {
            for (const auto& item : gizmo.segments)
            {
                if (item.axis == axis)
                    return item.segment.end;
            }
            return glm::vec2{};
        };
        const glm::vec2 beforeYEnd = findAxisStemEnd(scaleBefore, Editor::GizmoAxis::Y);
        const glm::vec2 afterXEnd = findAxisStemEnd(scaleAfter, Editor::GizmoAxis::X);
        EXPECT_NEAR(afterXEnd.x, beforeYEnd.x, 1e-2f);
        EXPECT_NEAR(afterXEnd.y, beforeYEnd.y, 1e-2f);

        // 命中测试：轴干、末端、旋转圆环与容差内外边界检查。
        const glm::vec2 xTip = findAxisStemEnd(translateBefore, Editor::GizmoAxis::X);
        const glm::vec2 yMid = (translateBefore.segments[3].segment.start + findAxisStemEnd(translateBefore, Editor::GizmoAxis::Y)) * 0.5f;
        EXPECT_EQ(Editor::pickGizmoAxis(xTip, translateBefore, 8.0f), Editor::GizmoAxis::X);
        EXPECT_EQ(Editor::pickGizmoAxis(yMid + glm::vec2(7.5f, 0.0f), translateBefore, 8.0f), Editor::GizmoAxis::Y);
        EXPECT_EQ(Editor::pickGizmoAxis(yMid + glm::vec2(8.5f, 0.0f), translateBefore, 8.0f), Editor::GizmoAxis::None);

        // 原点多轴重叠等距时按 X -> Y -> Z 确定返回 X。
        EXPECT_EQ(Editor::pickGizmoAxis(translateBefore.segments.front().segment.start, translateBefore, 8.0f),
                  Editor::GizmoAxis::X);

        // Y 与 Z 在交点精确等距时返回 Y；Z 明显更近时返回 Z。
        Editor::ProjectedGizmo synthetic;
        synthetic.visible = true;
        synthetic.segments = {
            {Editor::GizmoAxis::Z, {{100.0f, 100.0f}, {150.0f, 100.0f}}},
            {Editor::GizmoAxis::Y, {{100.0f, 100.0f}, {100.0f, 150.0f}}},
        };
        EXPECT_EQ(Editor::pickGizmoAxis({100.0f, 100.0f}, synthetic, 8.0f), Editor::GizmoAxis::Y);
        EXPECT_EQ(Editor::pickGizmoAxis({130.0f, 103.0f}, synthetic, 8.0f), Editor::GizmoAxis::Z);

        // Rotate 圆环命中与无效输入安全返回（避开与 X/Y 圆环交叉的 0° 点，取 Z 环 45° 处线段）。
        glm::vec2 zRingPoint{};
        int zCount = 0;
        for (const auto& item : rotateBefore.segments)
        {
            if (item.axis == Editor::GizmoAxis::Z)
            {
                if (zCount == 6)
                {
                    zRingPoint = (item.segment.start + item.segment.end) * 0.5f;
                    break;
                }
                ++zCount;
            }
        }
        EXPECT_EQ(Editor::pickGizmoAxis(zRingPoint, rotateBefore, 8.0f), Editor::GizmoAxis::Z);
        EXPECT_EQ(Editor::pickGizmoAxis({std::numeric_limits<float>::quiet_NaN(), 0.0f}, rotateBefore, 8.0f),
                  Editor::GizmoAxis::None);
    }
    constexpr const char* GLTF = "models/minimal_gltf/model.gltf";
    constexpr const char* OBJ = "models/minimal_obj/model.obj";
    constexpr const char* PNG = "textures/direction_rgba.png";
    constexpr const char* DEMO_GLTF = "models/avocado/glTF/Avocado.gltf";
    constexpr const char* DEMO_OBJ = "models/utah_teapot/utah_teapot.obj";
    constexpr const char* DEMO_PNG = "models/avocado/glTF/Avocado_baseColor.png";

    struct Lifetime
    {
        bool ioDestroyed = false;
        bool routesEmptyAtIoClose = false;
        bool gpuAliveAtIoClose = false;
        bool windowAliveAtIoClose = false;
        bool callbackReleased = true;
        bool callbackReleasedAtIoClose = false;
        bool modelAliveAtIoClose = false;
        size_t opens = 0;
        HWND window = nullptr;
        Render::Mesh mesh;
        std::weak_ptr<const Model::PreparedModel> model;
    };

    class TrackedFileIO final : public NativeIO::FileIO
    {
    public:
        explicit TrackedFileIO(std::shared_ptr<Lifetime> state)
            : state_(std::move(state)), native_(NativeIO::createPlatformFileIO())
        {}
        ~TrackedFileIO() override
        {
            state_->ioDestroyed = true;
            state_->routesEmptyAtIoClose = ResourceManager::instance().loadersEmpty();
            state_->gpuAliveAtIoClose = state_->mesh.valid();
            state_->windowAliveAtIoClose = state_->window && IsWindow(state_->window);
            state_->callbackReleasedAtIoClose = state_->callbackReleased;
            state_->modelAliveAtIoClose = !state_->model.expired();
        }
        bool exists(const fs::path& path) override
        {
            return native_->exists(path);
        }
        uint64_t getFileSize(const fs::path& path) override
        {
            return native_->getFileSize(path);
        }
        bool remove(const fs::path& path) override
        {
            return native_->remove(path);
        }
        std::unique_ptr<NativeIO::FileHandle> open(const fs::path& path, NativeIO::OpenMode mode,
                                                   NativeIO::ShareMode share) override
        {
            ++state_->opens;
            return native_->open(path, mode, share);
        }
        std::vector<NativeIO::DirectoryEntry> listDirectory(const std::filesystem::path& path) override
        {
            return native_->listDirectory(path);
        }
        bool directoryExists(const fs::path& path) override
        {
            return native_->directoryExists(path);
        }
        bool createDirectory(const fs::path& path) override
        {
            return native_->createDirectory(path);
        }
        bool removeDirectory(const fs::path& path) override
        {
            return native_->removeDirectory(path);
        }

    private:
        std::shared_ptr<Lifetime> state_;
        std::unique_ptr<NativeIO::FileIO> native_;
    };

    class OriginalResource final : public ResourceObject
    {
    public:
        OriginalResource(ResourceId id, ResourceType type) : ResourceObject(id, type) {}
    };

    class OriginalLoader final : public LoaderBase
    {
    public:
        OriginalLoader(ResourceType type, std::shared_ptr<bool> destroyed)
            : type_(type), destroyed_(std::move(destroyed))
        {}
        ~OriginalLoader() override
        {
            *destroyed_ = true;
        }
        std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view) override
        {
            if (type_ == ResourceType::TEXT)
                return std::make_unique<TextResource>(id, "original route");
            return std::make_unique<OriginalResource>(id, type_);
        }

    private:
        ResourceType type_;
        std::shared_ptr<bool> destroyed_;
    };

    class EditorAppTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            previous_ = fs::current_path();
            sandbox_ =
                previous_ / "review/application" / ::testing::UnitTest::GetInstance()->current_test_info()->name();
            fs::remove_all(sandbox_);
            fs::create_directories(sandbox_ / "assets");
            const auto fixtureRoot = Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR);
            for (const char* directory : {"models", "textures", "shaders", "fonts"})
                fs::copy(fixtureRoot / directory, sandbox_ / "assets" / directory,
                         fs::copy_options::recursive);
            fs::current_path(sandbox_);
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(renderLog_);
            sink->set_pattern("%l|%v");
            for (const char* name : {"render", "render.vulkan"})
            {
                Log::Registry::get(name).addSink(sink);
                Log::Registry::get(name).setLevel(Log::Level::DEBUG);
            }
        }
        void TearDown() override
        {
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            fs::current_path(previous_);
        }
        void copyShowcaseModels()
        {
            for (const char* directory : {"avocado", "utah_teapot", "ground"})
                fs::copy(previous_ / "assets/models" / directory, sandbox_ / "assets/models" / directory,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            fs::create_directories(sandbox_ / "assets/scenes");
            for (const char* file : {"mixed.json", "shadows.json"})
                fs::copy_file(previous_ / "assets/scenes" / file, sandbox_ / "assets/scenes" / file,
                              fs::copy_options::overwrite_existing);
        }
        App::SessionOptions options(bool visible = false)
        {
            App::SessionOptions result;
            result.window.title = "Incubator application session";
            result.window.width = 640;
            result.window.height = 480;
            result.window.visible = visible;
            result.validation = Render::RendererValidationMode::Required;
            result.clearColor = {0.2f, 0.24f, 0.28f, 1};
            return result;
        }
        void track(Editor::EditorApp& session, const std::shared_ptr<const Model::PreparedModel>& model,
                   const std::shared_ptr<Lifetime>& state)
        {
            state->window = glfwGetWin32Window(static_cast<GLFWwindow*>(session.window().getNativeHandle()));
            state->model = model;
            state->mesh = model->meshes()[0];
        }
        void closed(const std::shared_ptr<Lifetime>& state)
        {
            EXPECT_TRUE(state->ioDestroyed);
            EXPECT_TRUE(state->routesEmptyAtIoClose);
            EXPECT_FALSE(state->gpuAliveAtIoClose);
            EXPECT_FALSE(state->windowAliveAtIoClose);
            EXPECT_TRUE(state->callbackReleasedAtIoClose);
            EXPECT_FALSE(state->modelAliveAtIoClose);
            EXPECT_TRUE(ResourceManager::instance().loadersEmpty());
        }
        void unloaded(ResourceType type, const char* path)
        {
            auto& registry = ResourceRegistry::instance();
            const auto id = registry.lookup(type, path);
            ASSERT_TRUE(id.valid());
            EXPECT_EQ(registry.refCount(id), 0u);
            EXPECT_EQ(registry.getObject(id), nullptr);
        }
        void routes()
        {
            const auto& manager = ResourceManager::instance();
            EXPECT_TRUE(manager.hasLoader(ResourceType::SHADER));
            EXPECT_TRUE(manager.hasLoader(ResourceType::TEXTURE));
            EXPECT_TRUE(manager.hasLoader(ResourceType::MODEL));
        }
        void cleanValidation()
        {
            const auto text = renderLog_.str();
            std::cout << text;
            EXPECT_NE(text.find("validation: enabled"), std::string::npos);
            EXPECT_EQ(text.find("warning|"), std::string::npos);
            EXPECT_EQ(text.find("error|"), std::string::npos);
        }
        size_t count(std::string_view value)
        {
            const auto text = renderLog_.str();
            size_t result = 0;
            for (auto pos = text.find(value); pos != std::string::npos; pos = text.find(value, pos + value.size()))
                ++result;
            return result;
        }
        fs::path previous_, sandbox_;
        std::ostringstream renderLog_;
    };

    TEST_F(EditorAppTest, AssetBrowserRefreshesWithoutLoadingAndDistinguishesFailures)
    {
        copyShowcaseModels();
        fs::create_directories("assets/browser/empty");
        for (const char* name : {"new.obj", "image.png", "data.bin", "material.mtl", "notes.txt"})
            std::ofstream(std::string("assets/browser/") + name) << "discovery only";
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
        session.run({[&](auto& app) {
                         Editor::EditorScene editor;
                         editor.initialize(app);
                         const auto opens = state->opens;
                         const auto prepared = editor.cachedModels();
                         const auto gpu = Render::Detail::RendererReviewAccess::resourceCounts(app.renderer());
                         ASSERT_TRUE(editor.browseDirectory("browser"));
                         ASSERT_EQ(editor.assetEntries().size(), 6u);
                         const auto kind = [&](std::string_view name) {
                             const auto& entries = editor.assetEntries();
                             return std::ranges::find(entries, name, &AssetEntry::name)->kind;
                         };
                         EXPECT_EQ(kind("new.obj"), AssetKind::Model);
                         EXPECT_EQ(kind("image.png"), AssetKind::Texture);
                         EXPECT_EQ(kind("data.bin"), AssetKind::Dependency);
                         EXPECT_EQ(kind("material.mtl"), AssetKind::Dependency);
                         EXPECT_EQ(kind("notes.txt"), AssetKind::File);
                         EXPECT_EQ(kind("empty"), AssetKind::Directory);
                         fs::remove("assets/browser/new.obj");
                         std::ofstream("assets/browser/extra.gltf") << "not parsed";
                         ASSERT_TRUE(editor.browseDirectory(editor.assetDirectory()));
                         EXPECT_EQ(kind("extra.gltf"), AssetKind::Model);
                         EXPECT_EQ(std::ranges::find(editor.assetEntries(), "new.obj", &AssetEntry::name),
                                   editor.assetEntries().end());
                         EXPECT_FALSE(ResourceManager::instance().query(ResourceType::MODEL, "browser/extra.gltf"));
                         EXPECT_FALSE(ResourceManager::instance().query(ResourceType::TEXTURE, "browser/image.png"));
                         ASSERT_TRUE(editor.browseDirectory("browser/empty"));
                         EXPECT_TRUE(editor.assetEntries().empty());
                         EXPECT_TRUE(editor.assetError().empty());
                         ASSERT_TRUE(editor.parentDirectory());
                         EXPECT_EQ(editor.assetDirectory(), "browser");
                         ASSERT_FALSE(editor.browseDirectory("browser/missing"));
                         EXPECT_FALSE(editor.assetError().empty());
                         EXPECT_NE(editor.assetError().find("browser/missing"), std::string::npos);
                         EXPECT_TRUE(editor.assetEntries().empty());
                         ASSERT_TRUE(editor.parentDirectory());
                         EXPECT_TRUE(editor.assetDirectory().empty());
                         EXPECT_TRUE(editor.assetError().empty());
                         EXPECT_FALSE(editor.assetEntries().empty());
                         EXPECT_EQ(state->opens, opens);
                         EXPECT_EQ(editor.cachedModels(), prepared);
                         const auto after = Render::Detail::RendererReviewAccess::resourceCounts(app.renderer());
                         EXPECT_EQ(after.meshes, gpu.meshes);
                         EXPECT_EQ(after.textures, gpu.textures);
                         EXPECT_EQ(after.materials, gpu.materials);
                         app.requestExit();
                     },
                     {},
                     {}});
        cleanValidation();
    }

    TEST_F(EditorAppTest, NewVirtualModelsShareAndFailuresPreserveEditingStateUntilRetry)
    {
        copyShowcaseModels();
        fs::copy("assets/models/minimal_gltf", "assets/models/imported_gltf", fs::copy_options::recursive);
        fs::copy("assets/models/minimal_obj", "assets/models/imported_obj", fs::copy_options::recursive);
        std::ostringstream failures;
        Editor::EditorApp session(options());
        session.run(
            {[&](auto& app) {
                 Editor::EditorScene editor;
                 editor.initialize(app);
                 editor.select(app.scene().entities()[0].id);
                 auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(failures);
                 sink->set_pattern("%l|%v");
                 Log::Registry::get("resource").addSink(sink);
                 Log::Registry::get("editor").addSink(sink);
                 const auto retry = [&](const char* model, const char* dependency, const char* reason) {
                     SCOPED_TRACE(model);
                     const auto selected = editor.selected();
                     const auto size = app.scene().entities().size();
                     const auto cached = editor.cachedModels();
                     const auto gpu = Render::Detail::RendererReviewAccess::resourceCounts(app.renderer());
                     const std::string file(editor.scenePath());
                     failures.str("");
                     failures.clear();
                     EXPECT_FALSE(editor.addModel(app, model));
                     EXPECT_EQ(editor.selected(), selected);
                     EXPECT_EQ(editor.scenePath(), file);
                     EXPECT_EQ(app.scene().entities().size(), size);
                     EXPECT_EQ(editor.cachedModels(), cached);
                     EXPECT_NE(editor.message().find(model), std::string::npos);
                     EXPECT_NE(editor.message().find(reason), std::string::npos) << editor.message();
                     const auto logged = failures.str();
                     const auto at = logged.find("error|");
                     ASSERT_NE(at, std::string::npos);
                     EXPECT_EQ(logged.find("error|", at + 6), std::string::npos) << logged;
                     const auto info = ResourceManager::instance().query(ResourceType::MODEL, model);
                     ASSERT_TRUE(info);
                     EXPECT_FALSE(info->loaded);
                     EXPECT_EQ(info->references, 0u);
                     const auto after = Render::Detail::RendererReviewAccess::resourceCounts(app.renderer());
                     EXPECT_EQ(after.meshes, gpu.meshes);
                     EXPECT_EQ(after.textures, gpu.textures);
                     EXPECT_EQ(after.materials, gpu.materials);
                     fs::remove(dependency);
                     fs::rename(std::string(dependency) + ".backup", dependency);
                     ASSERT_TRUE(editor.addModel(app, model)) << editor.message();
                     const auto first = editor.selected();
                     ASSERT_NE(app.scene().find(first), nullptr);
                     EXPECT_EQ(app.scene().find(first)->name, "model");
                     const auto shared = app.scene().find(first)->model;
                     EXPECT_EQ(shared->source().path(), model);
                     ASSERT_TRUE(editor.addModel(app, model));
                     EXPECT_NE(first, editor.selected());
                     EXPECT_EQ(app.scene().find(editor.selected())->model, shared);
                     EXPECT_EQ(editor.cachedModels(), cached + 1);
                 };
                 const char* bin = "assets/models/imported_gltf/geometry/mesh.bin";
                 fs::rename(bin, std::string(bin) + ".backup");
                 retry("models/imported_gltf/model.gltf", bin, "mesh.bin");
                 const char* mtl = "assets/models/imported_obj/materials/model.mtl";
                 fs::rename(mtl, std::string(mtl) + ".backup");
                 retry("models/imported_obj/model.obj", mtl, "model.mtl");
                 fs::copy("assets/models/minimal_gltf", "assets/models/bad_json", fs::copy_options::recursive);
                 const char* json = "assets/models/bad_json/model.gltf";
                 fs::copy_file(json, std::string(json) + ".backup");
                 std::ofstream(json) << "{invalid";
                 retry("models/bad_json/model.gltf", json, "Invalid glTF JSON");
                 fs::copy("assets/models/minimal_gltf", "assets/models/missing_png", fs::copy_options::recursive);
                 fs::rename(std::string("assets/") + PNG, std::string("assets/") + PNG + ".backup");
                 retry("models/missing_png/model.gltf", "assets/textures/direction_rgba.png", "direction_rgba.png");
                 ASSERT_TRUE(editor.save(app, "new-models.json"));
                 const auto description = Scene::readScene(app.fileIO(), "new-models.json");
                 EXPECT_EQ(description.entities.back().model->path(), "models/missing_png/model.gltf");
                 EXPECT_EQ(description.entities.back().model->options(),
                           app.scene().entities().back().model->source().options());
                 app.requestExit();
             },
             {},
             {}});
        cleanValidation();
    }

    TEST_F(EditorAppTest, NameFieldEditsSelectedEntityWithoutPreparingAnotherModel)
    {
        copyShowcaseModels();
        auto state = std::make_shared<Lifetime>();
        auto config = options(true);
        config.window.width = 1280;
        config.window.height = 800;
        Editor::EditorApp session(config, std::make_unique<TrackedFileIO>(state));
        auto editor = std::make_shared<Editor::EditorScene>();
        Scene::EntityId selected;
        Scene::EntityId first;
        std::shared_ptr<const Model::PreparedModel> model;
        size_t opens = 0;
        int frame = 0;
        ImVec2 click{};
        ImVec2 firstRow{}, thirdRow{};
        session.run({[&](auto& app) {
                         editor->initialize(app);
                         first = app.scene().entities()[0].id;
                         selected = editor->createEntity(app);
                         ASSERT_TRUE(selected.valid());
                         EXPECT_EQ(app.scene().find(selected)->name, "Avocado");
                         model = app.scene().find(selected)->model;
                         opens = state->opens;
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         glfwSetCursorPosCallback(handle, nullptr);
                         const auto enter = glfwSetCursorEnterCallback(handle, nullptr);
                         enter(handle, GLFW_TRUE);
                         const auto focus = glfwSetWindowFocusCallback(handle, nullptr);
                         glfwSetWindowFocusCallback(handle, focus);
                         focus(handle, GLFW_TRUE);
                         glfwFocusWindow(handle);
                     },
                     [&](auto& app, const auto&) {
                         auto& io = ImGui::GetIO();
                         if (frame == 3)
                         {
                             io.AddMousePosEvent(click.x, click.y);
                             io.AddMouseButtonEvent(0, true);
                         }
                         if (frame == 4)
                             io.AddMouseButtonEvent(0, false);
                         if (frame == 6)
                             io.AddInputCharactersUTF8(" 兔子##");
                         if (frame == 8)
                         {
                             ASSERT_NE(app.scene().find(selected)->name.find("兔子##"), std::string::npos);
                             app.scene().setName(first, app.scene().find(selected)->name);
                             ImGui::SetWindowFocus("Objects");
                         }
                         if (frame == 10 || frame == 13)
                         {
                             const auto position = frame == 10 ? firstRow : thirdRow;
                             io.AddMousePosEvent(position.x, position.y);
                             io.AddMouseButtonEvent(0, true);
                         }
                         if (frame == 11 || frame == 14)
                             io.AddMouseButtonEvent(0, false);
                         if (frame == 12)
                             EXPECT_EQ(editor->selected(), first);
                         if (frame == 17)
                         {
                             EXPECT_NE(app.scene().find(selected)->name.find("兔子##"), std::string::npos);
                             EXPECT_EQ(editor->selected(), selected);
                             EXPECT_EQ(app.scene().find(selected)->model, model);
                             EXPECT_EQ(state->opens, opens);
                             app.requestExit();
                         }
                     },
                     [&](auto& app) {
                         editor->ui(app);
                         const auto* panel = ImGui::FindWindowByName("Properties");
                         ASSERT_NE(panel, nullptr);
                         const auto start = panel->DC.CursorStartPos;
                         click = {panel->Pos.x + panel->Size.x - ImGui::GetFontSize() * 2,
                                  start.y + ImGui::GetFrameHeightWithSpacing() +
                                      2 * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() * 0.5f};
                         if (frame >= 9)
                         {
                             const auto* objects = ImGui::FindWindowByName("Objects");
                             ASSERT_NE(objects, nullptr);
                             ASSERT_EQ(objects->DC.ChildWindows.Size, 1);
                             const auto* child = objects->DC.ChildWindows[0];
                             firstRow = {child->DC.CursorStartPos.x + 10,
                                         child->DC.CursorStartPos.y + ImGui::GetTextLineHeight() * 0.5f};
                             thirdRow = {firstRow.x, firstRow.y + 2 * ImGui::GetTextLineHeightWithSpacing()};
                         }
                         ++frame;
                     },
                     {},
                     {},
                     [editor](auto& app) { editor->layout(app); }});
        model.reset();
        cleanValidation();
    }

    TEST_F(EditorAppTest, ViewportClickSelectsSharedInstancesAndEmptySpace)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 1280;
        config.window.height = 800;
        Editor::EditorApp session(config);
        Editor::EditorScene editor;
        std::array<glm::vec2, 3> points{};
        std::array<bool, 3> found{};
        Scene::EntityId first, second;
        int stage = 0, frames = 0;
        bool issued = false;
        session.run({[&](auto& app) {
                         editor.initialize(app);
                         EditorTest::setSharedEditorScene(app, editor, false);
                         first = app.scene().entities()[0].id;
                         second = app.scene().entities()[1].id;
                         editor.select({});
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         const auto enter = glfwSetCursorEnterCallback(handle, nullptr);
                         enter(handle, GLFW_TRUE);
                         const auto focus = glfwSetWindowFocusCallback(handle, nullptr);
                         glfwSetWindowFocusCallback(handle, focus);
                         focus(handle, GLFW_TRUE);
                         glfwFocusWindow(handle);
                     },
                     [&](auto& app, const auto&) {
                         if (frames == 3)
                             ImGui::SetWindowFocus("Objects");
                         if (++frames > 150)
                         {
                             ADD_FAILURE() << "输入批次未完成";
                             app.requestExit();
                             return;
                         }
                         const auto& region = app.viewportRegion();
                         if (frames < 5 || stage >= 6 || issued || region.size.x <= 0 || region.size.y <= 0)
                             return;
                         if (!found[0] || !found[1] || !found[2])
                         {
                             for (int y = 1; y < 20; ++y)
                                 for (int x = 1; x < 20; ++x)
                                 {
                                     const glm::vec2 p(x / 20.0f, y / 20.0f);
                                     const auto hit = app.scene().pick(app.sceneExtent(), p);
                                     const int index = !hit ? 2 : hit->id == first ? 0 : hit->id == second ? 1 : -1;
                                     if (index >= 0 && !found[index])
                                     {
                                         points[index] = p;
                                         found[index] = true;
                                     }
                                 }
                             if (!found[0] || !found[1] || !found[2])
                                 return;
                         }
                         auto& io = ImGui::GetIO();
                         const auto p = points[stage / 2];
                         io.AddMousePosEvent(float(region.origin.x + p.x * region.size.x),
                                             float(region.origin.y + p.y * region.size.y));
                         io.AddMouseButtonEvent(0, stage % 2 == 0);
                         issued = true;
                     },
                     [&](auto& app) { editor.ui(app); },
                     [&](auto& app) { editor.cancelInteraction(app); },
                     [&](auto& app, auto extent, const auto& input) {
                         editor.input(app, extent, input);
                         if (issued && ((stage % 2 == 0 && input.select.pressed) ||
                                        (stage % 2 == 1 && input.select.released)))
                         {
                             if (stage == 1) EXPECT_EQ(editor.selected(), first);
                             if (stage == 3) EXPECT_EQ(editor.selected(), second);
                             if (stage == 5) EXPECT_FALSE(editor.selected().valid());
                             ++stage;
                             issued = false;
                             if (stage == 6) app.requestExit();
                         }
                     },
                     [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(stage, 6);
        cleanValidation();
    }

    TEST_F(EditorAppTest, PopupBlocksViewportSelection)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 1280;
        config.window.height = 800;
        Editor::EditorApp session(config);
        Editor::EditorScene editor;
        Scene::EntityId selected;
        ImVec2 popupPoint{};
        int stage = 0, frames = 0;
        bool issued = false, opened = false;
        session.run({[&](auto& app) {
                         editor.initialize(app);
                         EditorTest::setSharedEditorScene(app, editor, false);
                         selected = editor.selected();
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         const auto enter = glfwSetCursorEnterCallback(handle, nullptr);
                         enter(handle, GLFW_TRUE);
                         const auto focus = glfwSetWindowFocusCallback(handle, nullptr);
                         glfwSetWindowFocusCallback(handle, focus);
                         focus(handle, GLFW_TRUE);
                         glfwFocusWindow(handle);
                     },
                     [&](auto& app, const auto&) {
                         if (++frames > 150)
                         {
                             ADD_FAILURE() << "弹窗输入批次未完成";
                             app.requestExit();
                         }
                         if (frames < 6 || issued || popupPoint.x == 0)
                             return;
                         auto& io = ImGui::GetIO();
                         io.AddMousePosEvent(popupPoint.x, popupPoint.y);
                         io.AddMouseButtonEvent(0, stage == 0);
                         issued = true;
                     },
                     [&](auto& app) {
                         editor.ui(app);
                         if (!opened)
                         {
                             ImGui::OpenPopup("Selection blocker");
                             opened = true;
                         }
                         if (ImGui::BeginPopupModal("Selection blocker", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                         {
                             ImGui::TextUnformatted("弹窗内点击不能选择场景");
                             const auto pos = ImGui::GetWindowPos();
                             const auto size = ImGui::GetWindowSize();
                             popupPoint = {pos.x + size.x * 0.5f, pos.y + size.y * 0.5f};
                             ImGui::EndPopup();
                         }
                     },
                     [&](auto& app) { editor.cancelInteraction(app); },
                     [&](auto& app, auto extent, const auto& input) {
                         editor.input(app, extent, input);
                         if (issued && ((stage == 0 && input.select.pressed) || (stage == 1 && input.select.released)))
                         {
                             EXPECT_TRUE(input.captureMouse);
                             EXPECT_EQ(editor.selected(), selected);
                             issued = false;
                             if (++stage == 2)
                                 app.requestExit();
                         }
                     },
                     [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(stage, 2);
        cleanValidation();
    }

    TEST_F(EditorAppTest, ClickCandidatesCancelAndUseLogicalCoordinatesAcrossExtents)
    {
        copyShowcaseModels();
        auto config = options();
        config.window.width = 1280;
        config.window.height = 800;
        Editor::EditorApp session(config);
        Editor::EditorScene editor;
        int frames = 0, updates = 0;
        session.run({[&](auto& app) {
                         editor.initialize(app);
                         EditorTest::setSharedEditorScene(app, editor, false);
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         const auto focus = glfwSetWindowFocusCallback(handle, nullptr);
                         glfwSetWindowFocusCallback(handle, focus);
                         focus(handle, GLFW_TRUE);
                         glfwFocusWindow(handle);
                     },
                     [&](auto& app, const auto&) {
                         if (++updates > 150)
                         {
                             ADD_FAILURE() << "未收到有效视口输入";
                             app.requestExit();
                         }
                     },
                     [&](auto& app) { editor.ui(app); }, {},
                     [&](auto& app, auto extent, const auto&) {
                         if (++frames != 3)
                             return;
                         const auto first = app.scene().entities()[0].id;
                         const auto second = app.scene().entities()[1].id;
                         const auto region = app.viewportRegion();
                         glm::vec2 point{};
                         bool found = false;
                         for (int y = 1; y < 30 && !found; ++y)
                             for (int x = 1; x < 30 && !found; ++x)
                             {
                                 const glm::vec2 p(x / 30.0f, y / 30.0f);
                                 const auto hit = app.scene().pick(extent, p);
                                 if (hit && hit->id == second)
                                 {
                                     point = p;
                                     found = true;
                                 }
                             }
                         if (!found)
                         {
                             ADD_FAILURE() << "未找到第二个实例的拾取点";
                             app.requestExit();
                             return;
                         }
                         App::UiInput press;
                         press.position = {point.x * region.size.x, point.y * region.size.y};
                         press.select = {true, true, false};
                         auto held = press;
                         held.select.pressed = false;
                         auto release = held;
                         release.select = {false, false, true};
                         const auto preserved = [&] { EXPECT_EQ(editor.selected(), first); };
                         editor.select(first);
                         // 图像外按下和越出后返回都不能形成有效点击。
                         auto outside = press;
                         outside.captureMouse = true;
                         editor.input(app, extent, outside);
                         editor.input(app, extent, release);
                         preserved();
                         editor.input(app, extent, press);
                         outside = held;
                         outside.captureMouse = true;
                         editor.input(app, extent, outside);
                         editor.input(app, extent, release);
                         preserved();
                         // 拖动超出阈值或同时操作相机后，即使返回原点也不能选择。
                         editor.input(app, extent, press);
                         auto dragged = held;
                         dragged.selectDragDistanceSquared = 10000;
                         editor.input(app, extent, dragged);
                         editor.input(app, extent, release);
                         preserved();
                         for (int button = 0; button < 2; ++button)
                         {
                             editor.input(app, extent, press);
                             auto camera = held;
                             (button == 0 ? camera.rotate : camera.pan) = {true, true, false};
                             editor.input(app, extent, camera);
                             editor.input(app, extent, release);
                             preserved();
                             editor.cancelInteraction(app);
                         }
                         editor.input(app, extent, press);
                         editor.cancelInteraction(app);
                         editor.input(app, extent, release);
                         preserved();
                         // 逻辑坐标不乘 DPI；等比例改变像素尺寸不改变命中。
                         for (float dpi : {1.0f, 1.5f, 2.0f})
                         {
                             editor.select(first);
                             const Render::FrameExtent pixels{uint32_t(extent.width * dpi),
                                                              uint32_t(extent.height * dpi)};
                             editor.input(app, pixels, press);
                             editor.input(app, pixels, release);
                             EXPECT_EQ(editor.selected(), second);
                         }
                         editor.select(first);
                         editor.input(app, extent, press);
                         EXPECT_FALSE(editor.load(app, "missing-scene.json"));
                         EXPECT_EQ(editor.selected(), first);
                         editor.input(app, extent, release);
                         EXPECT_EQ(editor.selected(), second);
                         editor.input(app, extent, press);
                         EXPECT_TRUE(editor.load(app, "assets/scenes/mixed.json"));
                         EXPECT_FALSE(editor.selected().valid());
                         editor.input(app, extent, release);
                         EXPECT_FALSE(editor.selected().valid());
                         app.requestExit();
                     },
                     [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(frames, 3);
        cleanValidation();
    }

    TEST_F(EditorAppTest, DuplicateButtonUsesSelectionOperation)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 1280;
        config.window.height = 900;
        Editor::EditorApp session(config);
        Editor::EditorScene editor;
        Scene::EntityId source;
        int frame = 0;
        session.run({[&](auto& app) {
                         editor.initialize(app);
                         EditorTest::setSharedEditorScene(app, editor, false);
                         source = editor.selected();
                     },
                     [&](auto& app, const auto&) {
                         if (++frame > 20)
                         {
                             ADD_FAILURE() << "未能触发复制按钮";
                             app.requestExit();
                         }
                     },
                     [&](auto& app) {
                         if (frame == 4)
                         {
                             auto* window = ImGui::FindWindowByName("Properties");
                             ASSERT_NE(window, nullptr);
                             const auto id = window->GetID("Duplicate");
                             auto& context = *ImGui::GetCurrentContext();
                             context.NavActivateId = id;
                             context.NavActivateDownId = id;
                         }
                         editor.ui(app);
                         if (frame == 4)
                         {
                             const auto copy = editor.selected();
                             EXPECT_NE(copy, source);
                             ASSERT_NE(app.scene().find(copy), nullptr);
                             EXPECT_EQ(app.scene().find(copy)->name, "未命名 副本");
                             EXPECT_EQ(app.scene().find(copy)->model, app.scene().find(source)->model);
                             app.requestExit();
                         }
                     },
                     {}, {}, [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(frame, 4);
        cleanValidation();
    }

    TEST_F(EditorAppTest, DuplicatePersistsValuesAndSharesCachedModel)
    {
        copyShowcaseModels();
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
        session.run({[&](auto& app) {
                         Editor::EditorScene editor;
                         editor.initialize(app);
                         const auto source = editor.createEntity(app);
                         ASSERT_TRUE(app.scene().setName(source, "模型 ##"));
                         const Scene::Transform pose({1, 2, 3}, glm::angleAxis(0.4f, glm::vec3(0, 1, 0)), {-1, 0, 2});
                         ASSERT_TRUE(app.scene().setTransform(source, pose));
                         ASSERT_TRUE(app.scene().setVisible(source, false));
                         const auto model = app.scene().find(source)->model;
                         const auto opens = state->opens;
                         const auto uploads = count("Uploaded mesh:");
                         const auto copy = editor.duplicateSelected(app);
                         ASSERT_TRUE(copy.valid());
                         EXPECT_NE(source, copy);
                         EXPECT_EQ(editor.selected(), copy);
                         EXPECT_EQ(app.scene().find(copy)->name, "模型 ## 副本");
                         EXPECT_EQ(app.scene().find(copy)->model, model);
                         EXPECT_EQ(state->opens, opens);
                         EXPECT_EQ(count("Uploaded mesh:"), uploads);
                         ASSERT_TRUE(editor.save(app, "duplicate.json"));
                         const auto description = Scene::readScene(app.fileIO(), "duplicate.json");
                         const auto& saved = description.entities.back();
                         EXPECT_EQ(saved.name, "模型 ## 副本");
                         ASSERT_TRUE(saved.model);
                         EXPECT_EQ(*saved.model, model->source());
                         EXPECT_EQ(saved.transform.matrix(), pose.matrix());
                         EXPECT_FALSE(saved.visible);
                         ASSERT_TRUE(editor.load(app, "duplicate.json"));
                         EXPECT_FALSE(editor.selected().valid());
                         EXPECT_EQ(app.scene().find(source), nullptr);
                         EXPECT_EQ(app.scene().find(copy), nullptr);
                         const auto& restored = app.scene().entities().back();
                         EXPECT_EQ(restored.name, "模型 ## 副本");
                         EXPECT_EQ(restored.transform.matrix(), pose.matrix());
                         EXPECT_FALSE(restored.visible);
                         EXPECT_EQ(restored.model, model);
                         EXPECT_EQ(count("Uploaded mesh:"), uploads);
                         app.requestExit();
                     }, [](auto& app, const auto&) { app.requestExit(); }, {}});
        cleanValidation();
    }

    TEST_F(EditorAppTest, SelectionOverlayFollowsSameFrameEditsOrderAndSavesReviewCaptures)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 1280;
        config.window.height = 800;
        Editor::EditorApp session(config);
        auto editor = std::make_shared<Editor::EditorScene>();
        Render::SceneOutput previous;
        WindowCapture::Image unselectedScene, selectedScene;
        Render::Detail::RendererReviewAccess::ResourceCounts baseline{};
        Scene::EntityId target;
        int frame = 0;

        auto captureScene = [&](Editor::EditorApp& app) {
            WindowCapture::Image image;
            const auto extent = previous.extent();
            image.width = int(extent.width);
            image.height = int(extent.height);
            Render::Detail::RendererReviewAccess::withSceneOutput(
                app.renderer(), previous, [&](const auto& device, const auto& targetImage) {
                    image.rgba = TextureGpuReview::captureMip(device, targetImage, 0, {extent.width, extent.height});
                });
            return image;
        };

        auto saveWorkspace = [&](Editor::EditorApp& app, const char* name) {
            glfwWaitEventsTimeout(0.06);
            WindowCapture::save(WindowCapture::capture(app.window()), std::string("review/") + name + ".ppm");
        };

        session.run(
            {[&](auto& app) {
                 editor->initialize(app, "assets/scenes/shadows.json");
                 app.shadowSettings().enabled = true;
                 ASSERT_GE(app.scene().entities().size(), 2u);
                 target = app.scene().entities()[0].id;
                 baseline = Render::Detail::RendererReviewAccess::resourceCounts(app.renderer());
             },
             [&](auto& app, const auto&) {
                 ++frame;
                 if (frame == 4)
                     unselectedScene = captureScene(app);
                 if (frame == 5)
                 {
                     // 选中辅助线仅绘制在 ImGui 叠加层，不改变离屏场景/阴影颜色路径。
                     selectedScene = captureScene(app);
                     EXPECT_EQ(selectedScene.rgba, unselectedScene.rgba);
                     saveWorkspace(app, "selection-overlay-initial");
                 }
                 if (frame == 6)
                     saveWorkspace(app, "selection-overlay-transformed");
                 if (frame == 7)
                     saveWorkspace(app, "selection-overlay-camera-popup");
                 if (frame == 9)
                 {
                     saveWorkspace(app, "selection-overlay-resized");
                 }
                 if (frame == 11)
                     app.requestExit();
             },
             [&](auto& app) {
                 auto* sceneList = app.sceneDrawList();
                 const int beforeVtx = sceneList ? sceneList->VtxBuffer.Size : 0;

                 if (frame == 4)
                 {
                     editor->select(target);
                 }
                 else if (frame == 5)
                 {
                     // 本帧编辑变换（移动/旋转/缩放）须立即反映到同一帧辅助线。
                     auto pose = app.scene().find(target)->transform;
                     pose.setPosition(pose.position() + glm::vec3(0.45f, 0.25f, 0.15f));
                     pose.setRotation(glm::angleAxis(glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                                      pose.rotation());
                     pose.setScale(pose.scale() * glm::vec3(1.25f, 0.9f, 1.15f));
                     ASSERT_TRUE(app.scene().setTransform(target, pose));
                 }
                 else if (frame == 6)
                 {
                     // 调整相机并打开浮动弹窗，验证辅助线随相机更新且停留在 Scene 层、不遮盖弹窗。
                     auto camera = app.scene().camera();
                     camera.setPosition(camera.position() + glm::vec3(0.8f, 0.4f, -0.8f));
                     app.scene().setCamera(camera);
                     editor->beginOpen();
                 }
                 else if (frame == 7)
                 {
                     editor->cancelFileAction();
                     // 隐藏选中实体：保留选择但不绘制辅助线。
                     ASSERT_TRUE(app.scene().setVisible(target, false));
                 }
                 else if (frame == 8)
                 {
                     // 恢复可见并调整窗口尺寸，验证尺寸变化后对齐。
                     ASSERT_TRUE(app.scene().setVisible(target, true));
                     auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                     glfwSetWindowSize(handle, 1024, 640);
                 }
                 else if (frame == 10)
                 {
                     // 删除选中实体：清除选择、停止绘制且界面辅助不写入场景文件。
                     editor->deleteSelected(app);
                     ASSERT_TRUE(editor->save(app, "overlay-saved.json"));
                     std::ifstream file("overlay-saved.json");
                     const std::string json((std::istreambuf_iterator<char>(file)), {});
                     EXPECT_EQ(json.find("selected"), std::string::npos);
                     EXPECT_EQ(json.find("overlay"), std::string::npos);
                 }

                 editor->ui(app);

                 const int afterVtx = sceneList ? sceneList->VtxBuffer.Size : 0;
                 // 不使用无裁限全窗口前景绘制。
                 EXPECT_EQ(ImGui::GetForegroundDrawList()->VtxBuffer.Size, 0);

                 if (frame == 3)
                 {
                     // 未选中时不追加辅助线顶点。
                     EXPECT_EQ(afterVtx, beforeVtx);
                 }
                 else if (frame == 4 || frame == 5 || frame == 6)
                 {
                     // 选中且可见时，在 Scene DrawList 的图像之后追加辅助线。
                     ASSERT_NE(sceneList, nullptr);
                     EXPECT_GT(afterVtx, beforeVtx);
                     const auto expected = Editor::projectSelectionBox(app.scene(), editor->selected(),
                                                                       app.sceneExtent(), app.viewportRegion());
                     EXPECT_FALSE(expected.empty());
                 }
                 else if (frame == 7)
                 {
                     // 隐藏后保留选择但不再绘制辅助线。
                     EXPECT_EQ(editor->selected(), target);
                     EXPECT_EQ(afterVtx, beforeVtx);
                 }
                 else if (frame == 10)
                 {
                     EXPECT_FALSE(editor->selected().valid());
                     EXPECT_EQ(afterVtx, beforeVtx);
                 }

                 // 不增加任何 GPU 渲染资源。
                 const auto counts = Render::Detail::RendererReviewAccess::resourceCounts(app.renderer());
                 EXPECT_EQ(counts.meshes, baseline.meshes);
                 EXPECT_EQ(counts.textures, baseline.textures);
                 EXPECT_EQ(counts.materials, baseline.materials);

                 previous = app.renderer().prepareSceneTarget(app.sceneExtent());
                 glfwWaitEventsTimeout(0.04);
             },
             [editor](auto& app) { editor->cancelInteraction(app); },
             [editor](auto& app, auto extent, const auto& input) { editor->input(app, extent, input); },
             [editor](auto& app) { editor->layout(app); }});

        cleanValidation();
    }

    TEST_F(EditorAppTest, MultiInstanceClickRenameDuplicateDeleteSaveAndLegacyCompatibility)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 1280;
        config.window.height = 800;
        Editor::EditorApp session(config);
        Editor::EditorScene editor;
        std::array<glm::vec2, 2> points{};
        std::array<bool, 2> found{};
        Scene::EntityId first, second, copy;
        std::shared_ptr<const Model::PreparedModel> sharedModel;
        int stage = 0, frames = 0;
        bool issued = false;

        session.run(
            {[&](auto& app) {
                 // 先验证旧 JSON v1（无 name 字段）可兼容加载且不保留选择。
                 editor.initialize(app, "assets/scenes/mixed.json");
                 EXPECT_FALSE(editor.selected().valid());
                 for (const auto& entity : app.scene().entities())
                     EXPECT_TRUE(entity.name.empty());

                 EditorTest::setSharedEditorScene(app, editor, false);
                 first = app.scene().entities()[0].id;
                 second = app.scene().entities()[1].id;
                 sharedModel = app.scene().find(first)->model;
                 EXPECT_EQ(app.scene().find(second)->model, sharedModel);
                 editor.select({});

                 auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                 const auto enter = glfwSetCursorEnterCallback(handle, nullptr);
                 enter(handle, GLFW_TRUE);
                 const auto focus = glfwSetWindowFocusCallback(handle, nullptr);
                 glfwSetWindowFocusCallback(handle, focus);
                 focus(handle, GLFW_TRUE);
                 glfwFocusWindow(handle);
             },
             [&](auto& app, const auto&) {
                 if (++frames > 150)
                 {
                     ADD_FAILURE() << "多实例交互流程未完成";
                     app.requestExit();
                     return;
                 }
                 const auto& region = app.viewportRegion();
                 if (frames < 5 || issued || region.size.x <= 0 || region.size.y <= 0)
                     return;
                 if (stage < 2 && (!found[0] || !found[1]))
                 {
                     for (int y = 1; y < 20; ++y)
                         for (int x = 1; x < 20; ++x)
                         {
                             const glm::vec2 p(x / 20.0f, y / 20.0f);
                             const auto hit = app.scene().pick(app.sceneExtent(), p);
                             const int index = hit && hit->id == first ? 0 : hit && hit->id == second ? 1 : -1;
                             if (index >= 0 && !found[index])
                             {
                                 points[index] = p;
                                 found[index] = true;
                             }
                         }
                     if (!found[0] || !found[1])
                         return;
                 }
                 if (stage == 0 || stage == 1 || stage == 3 || stage == 4)
                 {
                     const auto p = points[stage < 2 ? 0 : 1];
                     auto& io = ImGui::GetIO();
                     io.AddMousePosEvent(float(region.origin.x + p.x * region.size.x),
                                         float(region.origin.y + p.y * region.size.y));
                     io.AddMouseButtonEvent(0, stage == 0 || stage == 3);
                     issued = true;
                 }
             },
             [&](auto& app) {
                 editor.ui(app);
                 if (stage == 2)
                 {
                     ASSERT_EQ(editor.selected(), first);
                     ASSERT_TRUE(app.scene().setName(first, "主实例 ## 01"));
                     copy = editor.duplicateSelected(app);
                     ASSERT_TRUE(copy.valid());
                     EXPECT_EQ(editor.selected(), copy);
                     ASSERT_TRUE(app.scene().setTransform(
                         copy, Scene::Transform({0.0f, 1.8f, 0.0f}, {1, 0, 0, 0}, {0.5f, 0.5f, 0.5f})));
                     stage = 3;
                 }
                 else if (stage == 5)
                 {
                     ASSERT_EQ(editor.selected(), second);
                     editor.deleteSelected(app);
                     EXPECT_FALSE(editor.selected().valid());
                     EXPECT_EQ(app.scene().find(second), nullptr);
                     ASSERT_TRUE(editor.save(app, "multi-instance-saved.json"));
                     ASSERT_TRUE(editor.load(app, "multi-instance-saved.json"));
                     EXPECT_FALSE(editor.selected().valid());
                     EXPECT_EQ(app.scene().find(first), nullptr);
                     EXPECT_EQ(app.scene().find(copy), nullptr);
                     ASSERT_EQ(app.scene().entities().size(), 2u);
                     EXPECT_EQ(app.scene().entities()[0].name, "主实例 ## 01");
                     EXPECT_EQ(app.scene().entities()[1].name, "主实例 ## 01 副本");
                     EXPECT_EQ(app.scene().entities()[0].model, sharedModel);
                     EXPECT_EQ(app.scene().entities()[1].model, sharedModel);
                     EXPECT_EQ(app.scene().entities()[1].transform.position(), glm::vec3(0.0f, 1.8f, 0.0f));
                     ASSERT_TRUE(editor.load(app, "assets/scenes/mixed.json"));
                     EXPECT_FALSE(editor.selected().valid());
                     EXPECT_EQ(app.scene().entities().size(), 2u);
                     app.requestExit();
                 }
             },
             [&](auto& app) { editor.cancelInteraction(app); },
             [&](auto& app, auto extent, const auto& input) {
                 editor.input(app, extent, input);
                 if (issued && ((stage % 3 == 0 && input.select.pressed) || (stage % 3 == 1 && input.select.released)))
                 {
                     if (stage == 1)
                         EXPECT_EQ(editor.selected(), first);
                     if (stage == 4)
                         EXPECT_EQ(editor.selected(), second);
                     ++stage;
                     issued = false;
                 }
             },
             [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(stage, 5);
        cleanValidation();
    }

    TEST_F(EditorAppTest, DirtyStateTracksEditsWithoutFalsePositivesAndShowsStatusMarker)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 1280;
        config.window.height = 800;
        Editor::EditorApp session(config);
        Editor::EditorScene editor;
        int frame = 0;

        session.run({[&](auto& app) {
                         editor.initialize(app, "assets/scenes/mixed.json");
                         EXPECT_FALSE(editor.isDirty());
                         EXPECT_EQ(editor.statusText().find(" * |"), std::string::npos);
                     },
                     [&](auto& app, const auto&) {
                         ++frame;
                         if (frame == 2)
                         {
                             // 非持久化操作和失败操作均不得置脏。
                             editor.select(app.scene().entities()[0].id);
                             editor.focusSelected(app, app.sceneExtent());
                             editor.resetCamera(app);
                             app.shadowSettings().enabled = true;
                             app.shadowSettings().depthBias = 0.005f;
                             editor.browseDirectory("models");
                             editor.collectUnusedModels();
                             editor.restoreLayout();
                             EXPECT_FALSE(editor.addModel(app, "models/missing.gltf"));
                             EXPECT_FALSE(editor.load(app, "missing-scene.json"));
                             EXPECT_FALSE(editor.save(app, "missing-dir/out.json"));
                             EXPECT_FALSE(editor.isDirty());
                             EXPECT_EQ(editor.statusText().find(" * |"), std::string::npos);
                         }
                         else if (frame == 3)
                         {
                             // addModel 成功后置脏且状态栏展示 * 标记。
                             ASSERT_TRUE(editor.addModel(app, GLTF));
                             EXPECT_TRUE(editor.isDirty());
                             EXPECT_NE(editor.statusText().find("assets/scenes/mixed.json * |"), std::string::npos);
                         }
                         else if (frame == 4)
                         {
                             // save 成功后清除脏状态与 * 标记。
                             ASSERT_TRUE(editor.save(app, "dirty-check-saved.json"));
                             EXPECT_FALSE(editor.isDirty());
                             EXPECT_EQ(editor.statusText().find(" * |"), std::string::npos);
                         }
                         else if (frame == 5)
                         {
                             // 失败保存不破坏既有干净状态；真实修改置脏后 load 恢复干净状态。
                             EXPECT_FALSE(editor.save(app, "missing-dir/out.json"));
                             EXPECT_FALSE(editor.isDirty());
                             editor.select(app.scene().entities()[0].id);
                             editor.duplicateSelected(app);
                             EXPECT_TRUE(editor.isDirty());
                             EXPECT_NE(editor.statusText().find("dirty-check-saved.json * |"), std::string::npos);
                             ASSERT_TRUE(editor.load(app, "dirty-check-saved.json"));
                             EXPECT_FALSE(editor.isDirty());
                             EXPECT_EQ(editor.statusText().find(" * |"), std::string::npos);
                             app.requestExit();
                         }
                     },
                     [&](auto& app) { editor.ui(app); },
                     [&](auto& app) { editor.cancelInteraction(app); },
                     [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
                     [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(frame, 5);
        cleanValidation();
    }

    TEST_F(EditorAppTest, UnsavedChangesDialogGuardsOpenClearExitAndWindowCloseWithSaveDiscardCancel)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 1280;
        config.window.height = 800;
        Editor::EditorApp session(config);
        Editor::EditorScene editor;
        int frame = 0;

        auto activateWindowButton = [](const char* windowName, const char* buttonLabel) {
            auto* window = ImGui::FindWindowByName(windowName);
            ASSERT_NE(window, nullptr) << windowName;
            const auto id = window->GetID(buttonLabel);
            auto& context = *ImGui::GetCurrentContext();
            context.NavActivateId = id;
            context.NavActivateDownId = id;
        };

        session.run(
            {[&](auto& app) {
                 editor.initialize(app, "assets/scenes/mixed.json");
                 EXPECT_FALSE(editor.isDirty());
                 // 干净状态下触发 requestOpen 直接打开路径窗、requestClose 不拦截。
                 editor.requestClose(app);
                 EXPECT_FALSE(app.window().shouldClose());
                 editor.requestOpen(app);
                 EXPECT_EQ(ImGui::FindWindowByName("Unsaved changes"), nullptr);
                 editor.cancelFileAction();
             },
             [&](auto& app, const auto&) {
                 ++frame;
                 if (frame > 40)
                 {
                     ADD_FAILURE() << "未保存拦截测试未按预期推进";
                     app.requestExit();
                     return;
                 }
                 if (frame == 3)
                 {
                     // 置脏后触发 requestOpen -> 弹出 Unsaved changes。
                     ASSERT_TRUE(editor.addModel(app, GLTF));
                     EXPECT_TRUE(editor.isDirty());
                     editor.requestOpen(app);
                 }
                 else if (frame == 6)
                 {
                     EXPECT_EQ(ImGui::FindWindowByName("Open scene"), nullptr);
                     EXPECT_TRUE(editor.isDirty());
                     // 触发 requestClear -> 弹出 Unsaved changes。
                     editor.requestClear(app);
                 }
                 else if (frame == 9)
                 {
                     EXPECT_TRUE(app.scene().entities().empty());
                     // 恢复混合场景并保存为关联副本，再做修改后走 Save 继续原 PendingAction (Open)。
                     ASSERT_TRUE(editor.load(app, "assets/scenes/mixed.json"));
                     ASSERT_TRUE(editor.save(app, "guarded-work.json"));
                     ASSERT_TRUE(editor.addModel(app, GLTF));
                     EXPECT_TRUE(editor.isDirty());
                     editor.requestOpen(app);
                 }
                 else if (frame == 12)
                 {
                     EXPECT_FALSE(editor.isDirty());
                     EXPECT_NE(ImGui::FindWindowByName("Open scene"), nullptr);
                     editor.cancelFileAction();
                 }
                 else if (frame == 14)
                 {
                     app.requestExit();
                 }
             },
             [&](auto& app) {
                 if (frame == 5)
                     activateWindowButton("Unsaved changes", "Cancel");
                 else if (frame == 8)
                     activateWindowButton("Unsaved changes", "Discard");
                 else if (frame == 11)
                     activateWindowButton("Unsaved changes", "Save");
                 editor.ui(app);
             },
             [&](auto& app) { editor.cancelInteraction(app); },
             [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
             [&](auto& app) { editor.layout(app); },
             [&](auto& app) { editor.requestClose(app); }});
        EXPECT_EQ(frame, 14);

        // 第二个独立会话：验证无文件关联时，窗口关闭按钮 (closeRequested) 拦截 -> Save -> Save As Cancel 中止 -> 再触发 -> Save As 保存后退出。
        Editor::EditorApp closeSession(config);
        Editor::EditorScene closeEditor;
        int closeFrame = 0;
        bool closeSaved = false;
        closeSession.run(
            {[&](auto& app) {
                 closeEditor.initialize(app, "");
                 EXPECT_TRUE(closeEditor.scenePath().empty());
                 EXPECT_FALSE(closeEditor.isDirty());
                 ASSERT_TRUE(closeEditor.addModel(app, GLTF));
                 EXPECT_TRUE(closeEditor.isDirty());
                 EXPECT_NE(closeEditor.statusText().find("(unsaved) * |"), std::string::npos);
             },
             [&](auto& app, const auto&) {
                 ++closeFrame;
                 if (closeFrame > 40)
                 {
                     ADD_FAILURE() << "无文件关联窗口关闭保护测试未完成";
                     app.requestExit();
                     return;
                 }
                 if (closeFrame == 2)
                 {
                     // 通过原生 WM_CLOSE 投递窗口关闭消息，在 pollEvents() 后触发 AppSession::closeRequested 拦截退出并弹出 Unsaved changes。
                     auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                     PostMessageW(glfwGetWin32Window(handle), WM_CLOSE, 0, 0);
                 }
                 else if (closeFrame == 7)
                 {
                     EXPECT_TRUE(closeEditor.isDirty());
                     EXPECT_EQ(app.scene().entities().size(), 1u);
                     // 再次投递关闭按钮消息触发拦截。
                     auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                     PostMessageW(glfwGetWin32Window(handle), WM_CLOSE, 0, 0);
                 }
             },
             [&](auto& app) {
                 if (closeFrame == 4)
                     activateWindowButton("Unsaved changes", "Save");
                 else if (closeFrame == 6)
                 {
                     EXPECT_NE(ImGui::FindWindowByName("Save scene as"), nullptr);
                     activateWindowButton("Save scene as", "Cancel");
                 }
                 else if (closeFrame == 9)
                     activateWindowButton("Unsaved changes", "Save");
                 else if (closeFrame == 11)
                 {
                     closeEditor.operationPath("saved-on-close.json");
                     activateWindowButton("Save scene as", "Save");
                     closeSaved = true;
                 }
                 closeEditor.ui(app);
             },
             [&](auto& app) { closeEditor.cancelInteraction(app); },
             [&](auto& app, auto extent, const auto& input) { closeEditor.input(app, extent, input); },
             [&](auto& app) { closeEditor.layout(app); },
             [&](auto& app) { closeEditor.requestClose(app); }});
        EXPECT_TRUE(closeSaved);
        EXPECT_FALSE(closeEditor.isDirty());
        EXPECT_EQ(closeEditor.scenePath(), "saved-on-close.json");
        EXPECT_TRUE(fs::exists("saved-on-close.json"));
        cleanValidation();
    }

    TEST_F(EditorAppTest, HistoryStackReusesModelsProtectsCollectUnusedAndResetsOnLoad)
    {
        copyShowcaseModels();
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
        session.run(
            {[&](auto& app) {
                 Editor::EditorOperations ops;
                 ops.initialize(app.renderer());
                 auto initial = ops.load(app.fileIO(), "assets/scenes/mixed.json", true);
                 app.setScene(std::move(initial->scene));
                 app.clearColor() = initial->clearColor;
                 EXPECT_FALSE(ops.isDirty());
                 EXPECT_FALSE(ops.canUndo());
                 EXPECT_EQ(ops.cachedModels(), 2u);

                 // 加入新模型（minimal_gltf），随后删除它并检查 collectUnused 不会误回收仍在 undoStack 中的模型。
                 ASSERT_TRUE(ops.addModel(app.scene(), app.clearColor(), GLTF));
                 const auto addedId = ops.selected();
                 auto addedModel = app.scene().find(addedId)->model;
                 EXPECT_EQ(ops.cachedModels(), 3u);
                 EXPECT_TRUE(ops.isDirty());

                 const auto opensBeforeUndo = state->opens;
                 const auto uploadsBeforeUndo = count("Uploaded mesh:");

                 ASSERT_TRUE(ops.remove(app.scene(), app.clearColor()));
                 EXPECT_EQ(app.scene().find(addedId), nullptr);
                 // 此时 undoStack_ 仍持有 addedModel，显式清理不得回收它。
                 EXPECT_EQ(ops.collectUnusedModels(), 0u);
                 EXPECT_EQ(ops.cachedModels(), 3u);

                 // Undo 恢复被删除实体：不触发文件读取或 GPU 重新上传，且恢复原有 EntityId 与共享模型。
                 ASSERT_TRUE(ops.undo(app.scene(), app.clearColor()));
                 ASSERT_NE(app.scene().find(addedId), nullptr);
                 EXPECT_EQ(app.scene().find(addedId)->model, addedModel);
                 EXPECT_EQ(state->opens, opensBeforeUndo);
                 EXPECT_EQ(count("Uploaded mesh:"), uploadsBeforeUndo);

                 // 保存新基线 -> 再 Undo 到未添加模型之前，此时偏离保存基线故 isDirty 应为 true；Redo 回到保存基线恢复 false。
                 ASSERT_TRUE(ops.save(app.fileIO(), app.scene(), app.clearColor(), "history-saved.json"));
                 EXPECT_FALSE(ops.isDirty());
                 ASSERT_TRUE(ops.undo(app.scene(), app.clearColor()));
                 EXPECT_TRUE(ops.isDirty());
                 ASSERT_TRUE(ops.redo(app.scene(), app.clearColor()));
                 EXPECT_FALSE(ops.isDirty());

                 // 再删除并清空后用新分支编辑/加载重置历史栈，此时 collectUnusedModels 可正常回收模型。
                 ASSERT_TRUE(ops.remove(app.scene(), app.clearColor()));
                 ASSERT_TRUE(ops.undo(app.scene(), app.clearColor()));  // remove 进入 redoStack_
                 ASSERT_TRUE(ops.setVisible(app.scene(), app.clearColor(), false));  // 覆盖 redoStack_
                 EXPECT_FALSE(ops.canRedo());

                 // 失败的 load/save 不破坏既有历史栈。
                 EXPECT_FALSE(ops.load(app.fileIO(), "missing-scene.json"));
                 EXPECT_TRUE(ops.canUndo());
                 EXPECT_FALSE(ops.save(app.fileIO(), app.scene(), app.clearColor(), "missing-dir/bad.json"));
                 EXPECT_TRUE(ops.canUndo());

                 // 成功 load 清空历史栈与挂起分支，释放 undoStack_ 持有的 GLTF 模型，随后 collectUnusedModels 可回收之。
                 auto reloaded = ops.load(app.fileIO(), "assets/scenes/mixed.json");
                 ASSERT_TRUE(reloaded);
                 app.setScene(std::move(reloaded->scene));
                 app.clearColor() = reloaded->clearColor;
                 EXPECT_FALSE(ops.canUndo());
                 EXPECT_FALSE(ops.canRedo());
                 EXPECT_FALSE(ops.isDirty());
                 // 注意：局部变量 addedModel 仍在作用域内，先释放局部变量持有，再清理缓存。
                 addedModel.reset();
                 EXPECT_EQ(ops.collectUnusedModels(), 1u);
                 EXPECT_EQ(ops.cachedModels(), 2u);

                 // 验证历史容量上限淘汰：重新加入 GLTF 并删除后，连续提交 64 步新编辑将删除记录挤出 undoStack_，此时可正常回收。
                 ASSERT_TRUE(ops.addModel(app.scene(), app.clearColor(), GLTF));
                 const auto evictedTarget = ops.selected();
                 EXPECT_EQ(ops.cachedModels(), 3u);
                 ASSERT_TRUE(ops.remove(app.scene(), app.clearColor()));
                 EXPECT_EQ(ops.collectUnusedModels(), 0u);
                 ops.select(app.scene().entities().front().id);
                 for (size_t step = 0; step < Editor::EditorOperations::MAX_HISTORY; ++step)
                     ASSERT_TRUE(ops.setTransform(app.scene(), app.clearColor(),
                                                  Scene::Transform({static_cast<float>(step + 1), 0, 0})));
                 EXPECT_EQ(app.scene().find(evictedTarget), nullptr);
                 EXPECT_EQ(ops.collectUnusedModels(), 1u);
                 EXPECT_EQ(ops.cachedModels(), 2u);

                 app.requestExit();
             },
             {}, {}});
        cleanValidation();
    }

    TEST_F(EditorAppTest, ExplicitCacheCleanupRetainsSubmittedFramesAndLiveShadowModels)
    {
        copyShowcaseModels();
        Editor::EditorApp session(options());
        auto editor = std::make_shared<Editor::EditorScene>();
        int frame = 0;
        Render::Detail::RendererReviewAccess::ResourceCounts baseline{};
        std::weak_ptr<const Model::PreparedModel> removed;
        Render::Mesh mesh;
        Scene::EntityId source;
        session.run({[&](auto& app) {
                         editor->initialize(app, "assets/scenes/shadows.json");
                         app.shadowSettings().enabled = true;
                         baseline = Render::Detail::RendererReviewAccess::resourceCounts(app.renderer());
                     },
                     [&](auto& app, const auto&) {
                         if (++frame == 7)
                             app.requestExit();
                     },
                     [&](auto& app) {
                         editor->ui(app);
                         using Review = Render::Detail::RendererReviewAccess;
                         if (frame % 2 == 1)
                         {
                             ASSERT_TRUE(editor->addModel(app, GLTF));
                             const auto model = app.scene().find(editor->selected())->model;
                             source = editor->selected();
                             removed = model;
                             mesh = model->meshes()[0];
                             EXPECT_EQ(editor->cachedModels(), 4u);
                             const auto before = Review::resourceCounts(app.renderer());
                             const auto duplicate = editor->duplicateSelected(app);
                             ASSERT_TRUE(duplicate.valid());
                             EXPECT_EQ(app.scene().find(duplicate)->model, model);
                             EXPECT_EQ(app.scene().find(duplicate)->transform.matrix(),
                                       app.scene().find(source)->transform.matrix());
                             const auto after = Review::resourceCounts(app.renderer());
                             EXPECT_EQ(before.meshes, after.meshes);
                             EXPECT_EQ(before.textures, after.textures);
                             EXPECT_EQ(before.materials, after.materials);
                             EXPECT_EQ(editor->collectUnusedModels(), 0u);
                         }
                         else
                         {
                             editor->deleteSelected(app);
                             EXPECT_EQ(editor->collectUnusedModels(), 0u);
                             EXPECT_NE(app.scene().find(source), nullptr);
                             EXPECT_TRUE(mesh.valid());
                             editor->select(source);
                             editor->deleteSelected(app);
                             // 历史栈仍持有已删除实体的模型以支撑 Undo，显式清理不得误回收。
                             EXPECT_EQ(editor->collectUnusedModels(), 0u);
                             EXPECT_FALSE(removed.expired());
                             EXPECT_EQ(editor->cachedModels(), 4u);
                             EXPECT_TRUE(mesh.valid());
                             EXPECT_GT(Review::resourceCounts(app.renderer()).meshes, baseline.meshes);
                             auto snapshot =
                                 Scene::extractSceneFrame(app.scene(), {384, 256}, true, app.shadowSettings());
                             ASSERT_TRUE(snapshot);
                             EXPECT_FALSE(snapshot->shadowDraws.empty());
                             if (frame == 6)
                             {
                                 // 加载新场景清空历史栈，释放对 GLTF 与 ground 的历史持有；但上一帧已提交的在途 GPU 资源仍受帧槽保护。
                                 ASSERT_TRUE(editor->load(app, "assets/scenes/mixed.json"));
                                 EXPECT_EQ(editor->collectUnusedModels(), 2u);
                                 EXPECT_TRUE(removed.expired());
                                 EXPECT_EQ(editor->cachedModels(), 2u);
                                 EXPECT_TRUE(mesh.valid());
                                 Review::waitForFrames(app.renderer());
                                 EXPECT_FALSE(mesh.valid());

                                 editor->clear(app);
                                 // clear 动作已进入历史栈，avocado/teapot 仍受 Undo 持有保护。
                                 EXPECT_EQ(editor->collectUnusedModels(), 0u);
                                 ASSERT_TRUE(editor->save(app, "empty-history-reset.json"));
                                 ASSERT_TRUE(editor->load(app, "empty-history-reset.json"));
                                 EXPECT_EQ(editor->collectUnusedModels(), 2u);
                                 EXPECT_EQ(editor->cachedModels(), 0u);
                                 Review::waitForFrames(app.renderer());
                                 const auto empty = Review::resourceCounts(app.renderer());
                                 EXPECT_EQ(empty.meshes, 0u);
                                 EXPECT_EQ(empty.textures, 0u);
                                 EXPECT_EQ(empty.materials, 0u);
                             }
                         }
                     },
                     {},
                     {},
                     [editor](auto& app) { editor->layout(app); }});
        cleanValidation();
    }

    TEST_F(EditorAppTest, ShadowPreviewUsesCurrentFrameAndSurvivesFilesAndWindowRecovery)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 1920;
        config.window.height = 1080;
        Editor::EditorApp session(config);
        auto demo = std::make_shared<Editor::EditorScene>();
        Render::SceneOutput previous;
        std::array<std::shared_ptr<const Model::PreparedModel>, 3> models;
        WindowCapture::Image on, off, changedLight;
        std::set<VkImage> shadowImages;
        float previewBias = 0;
        int frame = 0, uiCallsWhileMinimized = 0;
        const auto difference = [](const WindowCapture::Image& a, const WindowCapture::Image& b) {
            size_t changed = 0;
            for (size_t at = 0; at + 3 < std::min(a.rgba.size(), b.rgba.size()); at += 4)
                if (std::abs(int(a.rgba[at]) - int(b.rgba[at])) > 2 ||
                    std::abs(int(a.rgba[at + 1]) - int(b.rgba[at + 1])) > 2 ||
                    std::abs(int(a.rgba[at + 2]) - int(b.rgba[at + 2])) > 2)
                    ++changed;
            return changed;
        };
        const auto capture = [&](Editor::EditorApp& app, const char* name) {
            WindowCapture::Image image;
            const auto extent = previous.extent();
            image.width = int(extent.width);
            image.height = int(extent.height);
            Render::Detail::RendererReviewAccess::withSceneOutput(
                app.renderer(), previous, [&](const auto& device, const auto& target) {
                    image.rgba = TextureGpuReview::captureMip(device, target, 0, {extent.width, extent.height});
                });
            Render::Detail::RendererReviewAccess::withShadowOutput(app.renderer(), previous,
                                                                   [&](const auto&, const auto& target) {
                                                                       shadowImages.insert(target.handle());
                                                                       EXPECT_EQ(target.extent().width, 1024u);
                                                                       EXPECT_EQ(target.extent().height, 1024u);
                                                                   });
            if (name)
            {
                WindowCapture::save(image, std::string("review/") + name + "-scene.ppm");
                glfwWaitEventsTimeout(0.06);
                WindowCapture::save(WindowCapture::capture(app.window()),
                                    std::string("review/") + name + "-workspace.ppm");
            }
            return image;
        };
        session.run({[&](auto& app) {
                         demo->initialize(app, "assets/scenes/shadows.json");
                         ASSERT_EQ(app.scene().entities().size(), 3u);
                         for (size_t i = 0; i < models.size(); ++i)
                             models[i] = app.scene().entities()[i].model;
                         app.shadowSettings().enabled = true;
                         previewBias = app.shadowSettings().depthBias;
                     },
                     [&](auto& app, const auto&) {
                         ++frame;
                         if (frame == 5)
                             on = capture(app, "shadow-on");
                         if (frame == 6)
                         {
                             off = capture(app, "shadow-off");
                             ASSERT_EQ(on.width, off.width);
                             ASSERT_EQ(on.height, off.height);
                             EXPECT_GT(difference(on, off), 500u);
                             size_t darker = 0;
                             for (size_t at = 0; at + 3 < on.rgba.size(); at += 4)
                                 if (int(off.rgba[at]) - int(on.rgba[at]) > 8)
                                     ++darker;
                             EXPECT_GT(darker, 500u);
                         }
                         if (frame == 7)
                         {
                             const auto again = capture(app, nullptr);
                             EXPECT_EQ(again.rgba, on.rgba);
                         }
                         if (frame == 8)
                         {
                             changedLight = capture(app, "changed-light");
                             EXPECT_GT(difference(on, changedLight), 500u);
                         }
                         if (frame == 9)
                             EXPECT_GT(difference(changedLight, capture(app, "moved-model")), 500u);
                         if (frame == 12)
                             EXPECT_EQ(capture(app, nullptr).rgba, on.rgba);
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         if (frame == 13)
                         {
                             glfwIconifyWindow(handle);
                             for (int attempt = 0; attempt < 100 && !app.window().isIconified(); ++attempt)
                                 glfwWaitEventsTimeout(0.005);
                             ASSERT_TRUE(app.window().isIconified());
                         }
                         if (frame == 16)
                         {
                             EXPECT_EQ(uiCallsWhileMinimized, 0);
                             glfwRestoreWindow(handle);
                             for (int attempt = 0; attempt < 100 && app.window().isIconified(); ++attempt)
                                 glfwWaitEventsTimeout(0.005);
                             ASSERT_FALSE(app.window().isIconified());
                             glfwSetWindowSize(handle, 1280, 720);
                             demo->restoreLayout();
                         }
                         if (frame == 19)
                         {
                             EXPECT_TRUE(previous.valid());
                             capture(app, "restored");
                             app.requestExit();
                         }
                     },
                     [&](auto& app) {
                         if (frame >= 13 && frame < 16)
                             ++uiCallsWhileMinimized;
                         demo->ui(app);
                         if (frame == 5)
                             app.shadowSettings().enabled = false;
                         if (frame == 6)
                             app.shadowSettings().enabled = true;
                         if (frame == 7)
                         {
                             auto lighting = app.scene().lighting();
                             lighting.direction = {-1, -0.9f, -0.2f};
                             app.scene().setLighting(lighting);
                         }
                         if (frame == 8)
                         {
                             const auto& entity = app.scene().entities()[0];
                             auto pose = entity.transform;
                             pose.setPosition(pose.position() + glm::vec3(0.4f, 0, 0.3f));
                             app.scene().setTransform(entity.id, pose);
                         }
                         if (frame == 9)
                         {
                             app.shadowSettings().depthBias = 0.002f;
                             ASSERT_TRUE(demo->save(app, "shadow-saved.json"));
                             std::ifstream file("shadow-saved.json");
                             const std::string json((std::istreambuf_iterator<char>(file)), {});
                             file.close();
                             EXPECT_EQ(json.find("\"shadow"), std::string::npos);
                             app.shadowSettings().enabled = false;
                             app.shadowSettings().depthBias = 0.003f;
                             ASSERT_TRUE(demo->load(app, "shadow-saved.json"));
                             EXPECT_EQ(demo->scenePath(), "shadow-saved.json");
                             EXPECT_FALSE(app.shadowSettings().enabled);
                             EXPECT_EQ(app.shadowSettings().depthBias, 0.003f);
                         }
                         if (frame == 10)
                         {
                             ASSERT_TRUE(demo->load(app, "assets/scenes/mixed.json"));
                             ASSERT_EQ(app.scene().entities().size(), 2u);
                             EXPECT_EQ(app.scene().entities()[0].model, models[0]);
                             EXPECT_EQ(app.scene().entities()[1].model, models[1]);
                             ASSERT_TRUE(demo->save(app, "mixed-copy.json"));
                             ASSERT_TRUE(demo->load(app, "mixed-copy.json"));
                             EXPECT_FALSE(app.shadowSettings().enabled);
                         }
                         if (frame == 11)
                         {
                             ASSERT_TRUE(demo->load(app, "assets/scenes/shadows.json"));
                             ASSERT_EQ(app.scene().entities().size(), 3u);
                             for (size_t i = 0; i < models.size(); ++i)
                                 EXPECT_EQ(app.scene().entities()[i].model, models[i]);
                             EXPECT_EQ(app.shadowSettings().depthBias, 0.003f);
                             app.shadowSettings().enabled = true;
                             app.shadowSettings().depthBias = previewBias;
                         }
                         previous = app.renderer().prepareSceneTarget(app.sceneExtent());
                         glfwWaitEventsTimeout(0.04);
                     },
                     [demo](auto& app) { demo->cancelInteraction(app); },
                     [demo](auto& app, auto extent, const auto& input) { demo->input(app, extent, input); },
                     [demo](auto& app) { demo->layout(app); }});
        EXPECT_EQ(shadowImages.size(), 2u);
        EXPECT_EQ(count("Created shadow target:"), 2u);
        cleanValidation();
    }

    TEST_F(EditorAppTest, MissingPreviewShadowShaderReportsRenderAndClosesSession)
    {
        fs::remove("assets/shaders/shadow.vert.spv");
        Editor::EditorApp session(options());
        try
        {
            session.run({[](auto& app) { app.shadowSettings().enabled = true; },
                         {},
                         {},
                         {},
                         {},
                         [](auto& app) { app.sceneViewport("Scene"); }});
            FAIL() << "Accepted missing shadow shader";
        }
        catch (const Error::Exception& error)
        {
            EXPECT_EQ(error.code(), Error::Code::RENDER);
            EXPECT_NE(error.message().find("shadow shader"), std::string::npos);
        }
        EXPECT_FALSE(session.isOpen());
        EXPECT_TRUE(ResourceManager::instance().loadersEmpty());
        cleanValidation();
    }

    TEST_F(EditorAppTest, FileAssociationIgnoresPendingPathAndFailedActions)
    {
        copyShowcaseModels();
        Editor::EditorApp session(options());
        session.run({[&](auto& app) {
                         Editor::EditorScene demo;
                         demo.initialize(app);
                         const std::string original(demo.scenePath());
                         demo.operationPath("unconfirmed.json");
                         ASSERT_TRUE(demo.save(app));
                         EXPECT_EQ(demo.scenePath(), original);
                         EXPECT_FALSE(fs::exists("unconfirmed.json"));
                         demo.beginSaveAs();
                         demo.operationPath("cancelled.json");
                         demo.cancelFileAction();
                         EXPECT_FALSE(demo.confirmFileAction(app));
                         EXPECT_EQ(demo.scenePath(), original);
                         demo.beginSaveAs();
                         demo.operationPath("missing-dir/file.json");
                         EXPECT_FALSE(demo.confirmFileAction(app));
                         EXPECT_EQ(demo.scenePath(), original);
                         demo.operationPath("saved-copy.json");
                         ASSERT_TRUE(demo.confirmFileAction(app));
                         EXPECT_EQ(demo.scenePath(), "saved-copy.json");
                         demo.beginOpen();
                         demo.operationPath("missing.json");
                         EXPECT_FALSE(demo.confirmFileAction(app));
                         EXPECT_EQ(demo.scenePath(), "saved-copy.json");
                         demo.operationPath(original);
                         ASSERT_TRUE(demo.confirmFileAction(app));
                         EXPECT_EQ(demo.scenePath(), original);
                         demo.clear(app);
                         EXPECT_EQ(demo.scenePath(), original);
                         app.requestExit();
                     },
                     {},
                     {}});
        cleanValidation();
    }

    TEST_F(EditorAppTest, WorkspacePersistsDockingAndResetKeepsScene)
    {
        copyShowcaseModels();
        for (int pass = 0; pass < 2; ++pass)
        {
            auto config = options(true);
            config.window.width = pass ? 1280 : 1920;
            config.window.height = pass ? 720 : 1080;
            Editor::EditorApp session(config);
            auto demo = std::make_shared<Editor::EditorScene>();
            int frame = 0;
            session.run({[demo](auto& app) {
                             demo->initialize(app);
                             demo->select(app.scene().entities()[0].id);
                         },
                         [&](auto& app, const auto&) {
                             if (++frame == 6)
                             {
                                 const auto image = WindowCapture::capture(app.window());
                                 WindowCapture::save(
                                     image, pass ? "review/workspace-restored.ppm" : "review/workspace-default.ppm");
                             }
                             if (frame == 8)
                                 app.requestExit();
                         },
                         [&, demo](auto& app) {
                             demo->ui(app);
                             if (frame == 2)
                                 ImGui::SetWindowFocus("Assets");
                             for (const char* name : {"Objects", "Assets", "Properties", "Scene"})
                             {
                                 const auto* panel = ImGui::FindWindowByName(name);
                                 ASSERT_NE(panel, nullptr);
                                 EXPECT_NE(panel->DockId, 0u) << name;
                             }
                             EXPECT_EQ(ImGui::FindWindowByName("Files"), nullptr);
                             EXPECT_EQ(ImGui::FindWindowByName("Status"), nullptr);
                             if (frame >= 5)
                             {
                                 const auto size = ImGui::GetMainViewport()->Size;
                                 EXPECT_GT(app.viewportRegion().size.y, size.y * 0.7f);
                                 EXPECT_GT(app.viewportRegion().size.x, size.x * 0.4f);
                             }
                             if (frame == 3)
                             {
                                 const auto count = app.scene().entities().size();
                                 demo->restoreLayout();
                                 EXPECT_EQ(app.scene().entities().size(), count);
                                 ImGui::SaveIniSettingsToDisk("incubator-editor-layout.ini");
                             }
                             glfwWaitEventsTimeout(0.04);
                         },
                         {},
                         {},
                         [demo](auto& app) { demo->layout(app); }});
            EXPECT_TRUE(fs::exists("incubator-editor-layout.ini"));
        }
        cleanValidation();
    }

    TEST_F(EditorAppTest, StartupConfigOpensCustomSceneAndTracksSuccessfulFileOperations)
    {
        auto io = NativeIO::createPlatformFileIO();
        Scene::SceneDescription content;
        content.entities.push_back({Model::ModelReference(GLTF), Scene::Transform({1, 2, 0}), false});
        content.entities.push_back({std::nullopt, Scene::Transform({4, 5, 6}), true});
        content.camera = Scene::Camera({0, 1, 9});
        content.lighting = {{0, -2, -3}, {0.2f, 0.6f, 0.8f}, 2, 0.3f};
        content.clearColor = {0.2f, 0.3f, 0.4f, 1};
        Scene::writeScene(*io, "custom-scene.json", content);
        {
            std::ofstream configFile("incubator.json");
            configFile << R"({"window":{"title":"配置窗口","width":640,"height":480},"startupScene":"custom-scene.json"})";
        }
        auto config = App::readStartupConfig(*io);
        config.options.window.visible = false;
        config.options.validation = Render::RendererValidationMode::Required;
        Editor::EditorApp session(config.options, std::move(io));
        session.run({[&](auto& app) {
                         Editor::EditorScene demo;
                         demo.initialize(app, config.startupScene);
                         EXPECT_EQ(demo.scenePath(), "custom-scene.json");
                         EXPECT_FALSE(demo.selected().valid());
                         ASSERT_EQ(app.scene().entities().size(), 2u);
                         EXPECT_EQ(app.scene().entities()[0].transform.position(), glm::vec3(1, 2, 0));
                         EXPECT_FALSE(app.scene().entities()[0].visible);
                         EXPECT_EQ(app.scene().entities()[1].model, nullptr);
                         EXPECT_EQ(app.scene().camera().position(), content.camera.position());
                         EXPECT_EQ(app.scene().lighting(), content.lighting);
                         EXPECT_EQ(app.clearColor(), content.clearColor);
                         int width, height;
                         app.window().getWindowSize(width, height);
                         EXPECT_EQ(width, 640);
                         EXPECT_EQ(height, 480);

                         EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::MODEL, DEMO_GLTF).valid());
                         EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::MODEL, DEMO_OBJ).valid());
                         demo.select(app.scene().entities()[0].id);
                         EXPECT_FALSE(demo.load(app, "missing-scene.json"));
                         EXPECT_EQ(demo.scenePath(), "custom-scene.json");
                         EXPECT_TRUE(demo.selected().valid());
                         ASSERT_TRUE(demo.save(app, "另存场景.json"));
                         EXPECT_EQ(demo.scenePath(), "另存场景.json");
                         ASSERT_TRUE(demo.save(app, demo.scenePath()));
                         EXPECT_FALSE(demo.save(app, "missing-directory/scene.json"));
                         EXPECT_EQ(demo.scenePath(), "另存场景.json");
                         ASSERT_TRUE(demo.load(app, "custom-scene.json"));
                         EXPECT_EQ(demo.scenePath(), "custom-scene.json");
                         EXPECT_FALSE(demo.selected().valid());
                         app.requestExit();
                     },
                     {},
                     {}});
        EXPECT_FALSE(session.isOpen());
        cleanValidation();
    }

    TEST_F(EditorAppTest, StartupSceneErrorsStopBeforeUpdateAndCloseSession)
    {
        auto io = NativeIO::createPlatformFileIO();
        Scene::SceneDescription description;
        description.entities.push_back({Model::ModelReference("models/missing.gltf"), Scene::Transform{}, true});
        Scene::writeScene(*io, "missing-model-scene.json", description);
        std::ofstream("invalid-scene.json") << "{}";
        for (const char* path : {"missing-scene.json", "invalid-scene.json", "missing-model-scene.json"})
        {
            SCOPED_TRACE(path);
            auto state = std::make_shared<Lifetime>();
            Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
            auto callbacks = Editor::makeCallbacks(path);
            int updates = 0;
            callbacks.update = [&](auto&, const auto&) { ++updates; };
            EXPECT_THROW(session.run(std::move(callbacks)), Error::Exception);
            EXPECT_EQ(updates, 0);
            EXPECT_FALSE(session.isOpen());
            closed(state);
        }
        cleanValidation();
    }

    TEST_F(EditorAppTest, SceneViewportUsesCurrentLayoutAndSurvivesHiddenFrames)
    {
        Editor::EditorApp session(options());
        int frame = 0;
        bool open = true;
        Render::FrameExtent first{};
        session.run({[&](auto& app) {
                         const auto model = Model::prepareModel(app.renderer(), GLTF);
                         app.scene().createEntity(model);
                         app.scene().setCamera(Scene::Camera({0, 0, 3}));
                         app.shadowSettings().enabled = true;
                     },
                     [&](auto& app, const auto&) {
                         if (++frame == 8)
                             app.requestExit();
                     },
                     [&](auto& app) {
                         if (frame == 1)
                             first = app.sceneExtent();
                         if (frame == 2)
                             EXPECT_NE(app.sceneExtent(), first);
                         if (frame == 3 || frame == 4)
                             EXPECT_EQ(app.sceneExtent(), Render::FrameExtent{});
                         if (frame >= 5)
                             EXPECT_GT(app.sceneExtent().width, 0u);
                         const auto entity = app.scene().entities()[0].id;
                         app.scene().setTransform(entity, Scene::Transform({float(frame) * 0.01f, 0, 0}));
                         ImGui::Begin("Other UI");
                         ImGui::Text("Frame %d", frame);
                         ImGui::End();
                     },
                     {},
                     {},
                     [&](auto& app) {
                         open = frame != 3;
                         if (frame > 1)
                             ImGui::SetWindowSize("Viewport", {float(400 + frame * 10), 300});
                         ImGui::SetWindowCollapsed("Viewport", frame == 4);
                         app.sceneViewport("Viewport", &open);
                     }});
        EXPECT_EQ(frame, 8);
        EXPECT_EQ(count("Created shadow target:"), 2u);
        cleanValidation();
    }

    TEST_F(EditorAppTest, OrbitFocusCommandsKeepAssetsAndEntitiesAndSynchronizeReplacement)
    {
        copyShowcaseModels();
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
        session.run({[&](Editor::EditorApp& app) {
                         Editor::EditorScene demo;
                         demo.initialize(app);
                         EditorTest::setSharedEditorScene(app, demo, false);
                         const auto id = demo.selected();
                         const auto transform = app.scene().find(id)->transform.matrix();
                         const auto opens = state->opens;
                         const auto camera = app.scene().camera().position();
                         App::UiInput input;
                         input.focusPressed = true;
                         input.captureKeyboard = true;
                         demo.input(app, {800, 600}, input);
                         EXPECT_EQ(app.scene().camera().position(), camera);
                         demo.select({});
                         EXPECT_FALSE(demo.focusSelected(app, {800, 600}));
                         demo.select(app.scene().createEntity());
                         EXPECT_FALSE(demo.focusSelected(app, {800, 600}));
                         demo.select(id);
                         input.captureKeyboard = false;
                         demo.input(app, {800, 600}, input);
                         EXPECT_NE(app.scene().camera().position(), camera);
                         EXPECT_EQ(app.scene().find(id)->transform.matrix(), transform);
                         demo.resetCamera(app);
                         EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 6));
                         app.scene().setCamera(Scene::Camera({0, 0, 8}));
                         EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 8));
                         demo.input(app, {800, 600}, {});
                         EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 8));
                         EditorTest::setSharedEditorScene(app, demo, false);
                         demo.input(app, {800, 600}, {});
                         EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 6));
                         EXPECT_EQ(app.scene().find(id), nullptr);
                         EXPECT_EQ(state->opens, opens);
                         app.requestExit();
                     },
                     {},
                     {}});
        cleanValidation();
    }

    TEST_F(EditorAppTest, WindowInputChainsWithImGuiAndDelayedBatchesSurviveRawFrameReset)
    {
        auto native = Window::createPlatformWindow({"Input integration", 640, 480, true, false});
        auto* handle = static_cast<GLFWwindow*>(native->getNativeHandle());
        const auto focus = glfwSetWindowFocusCallback(handle, nullptr);
        glfwSetWindowFocusCallback(handle, focus);
        focus(handle, GLFW_TRUE);
        {
            Render::Integration::ImGuiRenderer ui(*native);
            const auto mouse = glfwSetMouseButtonCallback(handle, nullptr);
            glfwSetMouseButtonCallback(handle, mouse);
            const auto position = glfwSetCursorPosCallback(handle, nullptr);
            glfwSetCursorPosCallback(handle, position);
            const auto key = glfwSetKeyCallback(handle, nullptr);
            glfwSetKeyCallback(handle, key);
            const auto scroll = glfwSetScrollCallback(handle, nullptr);
            glfwSetScrollCallback(handle, scroll);
            const auto character = glfwSetCharCallback(handle, nullptr);
            glfwSetCharCallback(handle, character);
            const auto uiFocus = glfwSetWindowFocusCallback(handle, nullptr);
            glfwSetWindowFocusCallback(handle, uiFocus);
            auto& io = ImGui::GetIO();
            io.DisplaySize = {640, 480};
            io.DeltaTime = 1.0f / 60;
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            App::UiInputAdapter adapter;
            int characters = 0;
            int focusPresses = 0;
            Input::PointerValue wheel;
            bool focusText = false;
            char text[32]{};
            const auto sample = [&] {
                ImGui::NewFrame();
                characters += io.InputQueueCharacters.Size;
                const auto result = adapter.sample(native->input());
                focusPresses += result.focusPressed;
                wheel.x += result.scroll.x;
                wheel.y += result.scroll.y;
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({200, 200});
                ImGui::Begin("Panel");
                ImGui::TextUnformatted("Input panel");
                if (focusText) ImGui::SetKeyboardFocusHere();
                ImGui::InputText("Text", text, sizeof(text));
                ImGui::End();
                ImGui::EndFrame();
                return result;
            };
            EXPECT_TRUE(io.ConfigInputTrickleEventQueue);
            position(handle, 100, 100);
            sample();
            sample();
            mouse(handle, 0, GLFW_PRESS, 0);
            sample();
            native->beginInputFrame();
            mouse(handle, 0, GLFW_RELEASE, 0);
            position(handle, 300, 300);
            mouse(handle, 1, GLFW_PRESS, 0);
            EXPECT_TRUE(native->input().mouseButton(Input::MouseButton::Right).pressed);
            EXPECT_FALSE(sample().rotate.pressed);
            native->beginInputFrame();
            EXPECT_FALSE(native->input().mouseButton(Input::MouseButton::Right).pressed);
            const auto delayed = sample();
            EXPECT_TRUE(delayed.rotate.pressed);
            EXPECT_FALSE(delayed.captureMouse);
            App::OrbitCamera orbit;
            Scene::Camera camera({0, 0, 6});
            EXPECT_FALSE(orbit.apply(camera, delayed, {640, 480}));
            position(handle, 320, 300);
            EXPECT_TRUE(orbit.apply(camera, sample(), {640, 480}));
            const auto dragged = camera.position();
            mouse(handle, 1, GLFW_RELEASE, 0);
            mouse(handle, 1, GLFW_PRESS, 0);
            mouse(handle, 1, GLFW_RELEASE, 0);
            EXPECT_TRUE(sample().rotate.released);
            native->beginInputFrame();
            EXPECT_TRUE(sample().rotate.pressed);
            EXPECT_TRUE(sample().rotate.released);
            key(handle, GLFW_KEY_F, 0, GLFW_PRESS, 0);
            scroll(handle, 1, 2);
            character(handle, 'x');
            for (int i = 0; i < 4; ++i) sample();
            EXPECT_EQ(characters, 1);
            EXPECT_EQ(focusPresses, 1);
            EXPECT_EQ(wheel, (Input::PointerValue{1, 2}));
            EXPECT_TRUE(native->input().key(Input::KeyCode::F).pressed);
            EXPECT_EQ(native->input().scroll(), (Input::PointerValue{1, 2}));
            native->pollEvents();
            native->pollEvents();
            EXPECT_EQ(native->input().scroll(), (Input::PointerValue{1, 2}));
            mouse(handle, 1, GLFW_PRESS, 0);
            mouse(handle, 1, GLFW_RELEASE, 0);
            key(handle, GLFW_KEY_F, 0, GLFW_RELEASE, 0);
            adapter.cancel();
            orbit.cancel();
            uiFocus(handle, GLFW_FALSE);
            uiFocus(handle, GLFW_TRUE);
            EXPECT_TRUE(native->input().focusLost());
            for (int i = 0; i < 5; ++i)
            {
                const auto canceled = sample();
                EXPECT_FALSE(canceled.rotate.pressed);
                EXPECT_FALSE(orbit.apply(camera, canceled, {640, 480}));
            }
            EXPECT_EQ(camera.position(), dragged);
            mouse(handle, 1, GLFW_PRESS, 0);
            EXPECT_TRUE(sample().rotate.pressed);
            mouse(handle, 1, GLFW_RELEASE, 0);
            position(handle, 100, 100);
            sample();
            sample();
            scroll(handle, 0, 1);
            EXPECT_TRUE(sample().captureMouse);
            focusText = true;
            sample();
            focusText = false;
            sample();
            key(handle, GLFW_KEY_F, 0, GLFW_PRESS, 0);
            const auto typing = sample();
            EXPECT_TRUE(typing.captureKeyboard);
            EXPECT_TRUE(typing.focusPressed);
        }
        native->beginInputFrame();
        focus(handle, GLFW_TRUE);
        const auto key = glfwSetKeyCallback(handle, nullptr);
        glfwSetKeyCallback(handle, key);
        key(handle, GLFW_KEY_A, 0, GLFW_PRESS, 0);
        EXPECT_TRUE(native->input().key(Input::KeyCode::A).pressed);
    }

    TEST_F(EditorAppTest, AnyExistingRouteRejectsTakeoverWithoutChangingOriginalLoader)
    {
        for (const auto type : {ResourceType::TEXT, static_cast<ResourceType>(0x12345678)})
        {
            auto& manager = ResourceManager::instance();
            auto destroyed = std::make_shared<bool>(false);
            manager.registerLoader(type, std::make_unique<OriginalLoader>(type, destroyed));
            auto state = std::make_shared<Lifetime>();
            try
            {
                Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
                FAIL() << "Accepted nonempty route table";
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::CONFIG);
            }
            EXPECT_TRUE(state->ioDestroyed);
            EXPECT_FALSE(state->routesEmptyAtIoClose);
            EXPECT_FALSE(*destroyed);
            EXPECT_TRUE(manager.hasLoader(type));
            EXPECT_FALSE(manager.hasLoader(ResourceType::SHADER));
            EXPECT_FALSE(manager.hasLoader(ResourceType::TEXTURE));
            EXPECT_FALSE(manager.hasLoader(ResourceType::MODEL));
            const auto id = manager.registerResource(type, "original.txt");
            const auto* object = manager.load(id);
            ASSERT_NE(object, nullptr);
            EXPECT_EQ(object->type(), type);
            if (type == ResourceType::TEXT)
                EXPECT_EQ(dynamic_cast<const TextResource*>(object)->text(), "original route");
            manager.release(id);
            manager.reset();
            EXPECT_TRUE(*destroyed);
        }
        EXPECT_EQ(count("Created Vulkan"), 0u);
    }

    TEST_F(EditorAppTest, ShaderInitializationFailureReleasesHoldsAndOwnedRoutesBeforeFileIO)
    {
        auto state = std::make_shared<Lifetime>();
        auto config = options();
        config.vertexShader = "shaders/missing.vert.spv";
        try
        {
            Editor::EditorApp session(config, std::make_unique<TrackedFileIO>(state));
            FAIL() << "Accepted missing Shader";
        }
        catch (const Error::Exception& error)
        {
            EXPECT_EQ(error.code(), Error::Code::RENDER);
        }
        closed(state);
        unloaded(ResourceType::SHADER, "shaders/missing.vert.spv");
        unloaded(ResourceType::SHADER, "shaders/cube.frag.spv");
        EXPECT_EQ(count("Created Vulkan"), 0u);
    }

    TEST_F(EditorAppTest, LiveRoutesAllowLaterModelPreparationWithOtherCpuOwner)
    {
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(options(true), std::make_unique<TrackedFileIO>(state));
        ResourceId otherPng;
        const TextureResource* otherPixels = nullptr;
        size_t ordinaryOpens = 0;
        session.run({[&](Editor::EditorApp& app) {
                         routes();
                         unloaded(ResourceType::SHADER, "shaders/cube.vert.spv");
                         unloaded(ResourceType::SHADER, "shaders/cube.frag.spv");
                         const auto model = Model::prepareModel(app.renderer(), GLTF);
                         track(app, model, state);
                         app.scene().createEntity(model);
                         unloaded(ResourceType::MODEL, GLTF);
                         unloaded(ResourceType::TEXTURE, PNG);
                         otherPng = ResourceRegistry::instance().lookup(ResourceType::TEXTURE, PNG);
                         ResourceManager::instance().acquire(otherPng);
                         otherPixels = ResourceManager::instance().get<TextureResource>(otherPng);
                         ASSERT_NE(otherPixels, nullptr);
                         ordinaryOpens = state->opens;
                     },
                     [&](Editor::EditorApp& app, const Editor::FrameTime&) {
                         routes();
                         EXPECT_EQ(state->opens, ordinaryOpens);
                         const auto model = Model::prepareModel(app.renderer(), OBJ);
                         app.scene().createEntity(model);
                         EXPECT_EQ(ResourceRegistry::instance().refCount(otherPng), 1u);
                         EXPECT_EQ(ResourceRegistry::instance().getObject(otherPng), otherPixels);
                         track(app, model, state);
                         unloaded(ResourceType::MODEL, OBJ);
                         // 会话中的其他 CPU 使用者不被模型准备释放；结束会话前须结束其持有，才能解绑资产源。
                         ResourceManager::instance().release(otherPng);
                         unloaded(ResourceType::TEXTURE, PNG);
                         app.requestExit();
                     },
                     {}});
        EXPECT_FALSE(session.isOpen());
        closed(state);
        unloaded(ResourceType::TEXTURE, PNG);
        EXPECT_EQ(ResourceManager::instance().assetSource(), nullptr);
        cleanValidation();
    }

    TEST_F(EditorAppTest, MinimizedLoopsAdvanceTimeAndUpdateWhileSkippingUiThenRestoreNewExtent)
    {
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(options(true), std::make_unique<TrackedFileIO>(state));
        std::vector<Editor::FrameTime> times;
        std::vector<int> uiUpdates;
        int updates = 0;
        size_t opens = 0;
        int cancellations = 0;
        session.run({[&](Editor::EditorApp& app) {
                         const auto model = Model::prepareModel(app.renderer(), GLTF);
                         track(app, model, state);
                         app.scene().setCamera(Scene::Camera({0, 0, 1}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 10}));
                         app.scene().createEntity(model, Scene::Transform({0, 0, 0}, {1, 0, 0, 0}, glm::vec3(0.5f)));
                         opens = state->opens;
                     },
                     [&](Editor::EditorApp& app, const Editor::FrameTime& time) {
                         const int frame = updates++;
                         times.push_back(time);
                         EXPECT_GE(time.delta, Clock::Duration::zero());
                         if (times.size() > 1)
                             EXPECT_EQ(time.delta, time.now - times[times.size() - 2].now);
                         app.scene().createEntity();  // 跳过渲染时逻辑仍会继续执行。
                         EXPECT_EQ(state->opens, opens);
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         if (frame == 1)
                         {
                             glfwIconifyWindow(handle);
                             for (int attempt = 0; attempt < 100 && !app.window().isIconified(); ++attempt)
                             {
                                 app.window().pollEvents();
                                 glfwWaitEventsTimeout(0.005);
                             }
                             ASSERT_TRUE(app.window().isIconified());
                         }
                         if (frame == 5)
                         {
                             EXPECT_EQ(uiUpdates, (std::vector<int>{0}));
                             EXPECT_EQ(app.scene().entities().size(), 7u);
                             glfwRestoreWindow(handle);
                             for (int attempt = 0; attempt < 100 && app.window().isIconified(); ++attempt)
                             {
                                 app.window().pollEvents();
                                 glfwWaitEventsTimeout(0.005);
                             }
                             ASSERT_FALSE(app.window().isIconified());
                             glfwSetWindowSize(handle, 800, 400);
                         }
                         if (frame == 10)
                         {
                             int width, height;
                             app.window().getFramebufferSize(width, height);
                             EXPECT_EQ(width, 800);
                             EXPECT_EQ(height, 400);
                             const auto restored = Scene::extractSceneFrame(
                                 app.scene(), {static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
                             ASSERT_TRUE(restored);
                             EXPECT_FALSE(restored->draws.empty());
                             app.requestExit();
                         }
                     },
                     [&](Editor::EditorApp& app) {
                         uiUpdates.push_back(updates - 1);
                         EXPECT_FALSE(app.window().isIconified());
                         glfwWaitEventsTimeout(0.04);
                     },
                     [&](Editor::EditorApp&) { ++cancellations; }});
        EXPECT_EQ(updates, 11);
        EXPECT_GE(cancellations, 4);
        EXPECT_EQ(uiUpdates, (std::vector<int>{0, 5, 6, 7, 8, 9}));
        closed(state);
        cleanValidation();
    }

    TEST_F(EditorAppTest, LightingWidgetsEditCurrentSceneAndSameFrameSnapshot)
    {
        auto state = std::make_shared<Lifetime>();
        auto config = options(true);
        config.window.height = 1000;
        Editor::EditorApp session(config, std::make_unique<TrackedFileIO>(state));
        const auto opens = state->opens;
        const Render::Lighting initial{{1, 2, -3}, {0.5f, 0.75f, 1}, 0.7f, 0.2f};
        const Render::Lighting external{{2, 1, -3}, {0.2f, 0.4f, 0.8f}, 0.4f, 0.3f};
        const Render::Lighting replacement{{-2, -3, -4}, {0.8f, 0.1f, 1}, 0.5f, 0.4f};
        ImVec2 intensityClick, yawClick;
        int frames = 0;
        Scene::SceneFrame retained;
        session.run({[&](Editor::EditorApp& app) {
                         app.scene().setLighting(initial);
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         glfwSetCursorPosCallback(handle, nullptr);
                         const auto enter = glfwSetCursorEnterCallback(handle, nullptr);
                         ASSERT_NE(enter, nullptr);
                         enter(handle, GLFW_TRUE);
                         glfwFocusWindow(handle);
                     },
                     [&](Editor::EditorApp& app, const Editor::FrameTime&) {
                         auto& io = ImGui::GetIO();
                         if (frames == 3 || frames == 5)
                         {
                             const auto position = frames == 3 ? intensityClick : yawClick;
                             io.AddMousePosEvent(position.x, position.y);
                             io.AddMouseButtonEvent(0, true);
                         }
                         if (frames == 4 || frames == 6)
                             io.AddMouseButtonEvent(0, false);
                         if (frames == 7)
                             app.scene().setLighting(external);
                         if (frames == 8)
                         {
                             auto scene = std::make_unique<Scene::Scene>();
                             scene->setLighting(replacement);
                             app.setScene(std::move(scene));
                         }
                         if (frames == 9)
                             app.requestExit();
                     },
                     [&](Editor::EditorApp& app) {
                         ImGui::SetNextWindowPos({12, 12}, ImGuiCond_Always);
                         // 面板需容纳 DPI 缩放后的五组标签和控件，避免把点击发到裁剪区。
                         const auto row = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing();
                         const auto height = 5 * row + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().WindowPadding.y;
                         ImGui::SetNextWindowSize({600, height}, ImGuiCond_Always);
                         ImGui::Begin("Lighting controls");
                         ImGui::PushItemWidth(200);
                         const auto origin = ImGui::GetCursorScreenPos();
                         yawClick = {origin.x + 50,
                                     origin.y + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() * 0.5f};
                         intensityClick = {origin.x + 150, yawClick.y + 3 * row};
                         App::lightingControls(app.scene());
                         ImGui::PopItemWidth();
                         ImGui::End();
                         const auto frame = Scene::extractSceneFrame(app.scene(), {800, 600});
                         ASSERT_TRUE(frame);
                         EXPECT_EQ(frame->input().lighting, app.scene().lighting());
                         if (frames == 3)
                         {
                             EXPECT_GT(frame->lighting.intensity, 2.0f);
                             EXPECT_EQ(frame->lighting.direction, initial.direction);
                             retained = *frame;
                         }
                         if (frames == 5)
                         {
                             EXPECT_NE(frame->lighting.direction, initial.direction);
                             EXPECT_NEAR(glm::length(frame->lighting.direction), 1, 0.00001f);
                             EXPECT_EQ(frame->lighting.intensity, retained.lighting.intensity);
                             EXPECT_TRUE(frame->lighting.valid());
                         }
                         if (frames == 7)
                             EXPECT_EQ(frame->lighting, external);
                         if (frames == 8)
                             EXPECT_EQ(frame->lighting, replacement);
                         EXPECT_EQ(state->opens, opens);
                         ++frames;
                     }});
        EXPECT_EQ(frames, 9);
        EXPECT_GT(retained.lighting.intensity, 2.0f);
        EXPECT_EQ(retained.lighting.direction, initial.direction);
        EXPECT_EQ(count("Uploaded mesh:"), 0u);
        EXPECT_EQ(count("Created material:"), 0u);
        closed(state);
        cleanValidation();
    }

    TEST_F(EditorAppTest, QueuedPanAndCanceledRotateUseConsumedInputBatches)
    {
        auto native = Window::createPlatformWindow({"Consumed input batches", 640, 480, false, false});
        auto* handle = static_cast<GLFWwindow*>(native->getNativeHandle());
        Render::Integration::ImGuiRenderer ui(*native);
        const auto mouse = glfwSetMouseButtonCallback(handle, nullptr);
        glfwSetMouseButtonCallback(handle, mouse);
        const auto position = glfwSetCursorPosCallback(handle, nullptr);
        glfwSetCursorPosCallback(handle, position);
        const auto focus = glfwSetWindowFocusCallback(handle, nullptr);
        glfwSetWindowFocusCallback(handle, focus);
        focus(handle, GLFW_TRUE);
        auto& io = ImGui::GetIO();
        io.DisplaySize = {640, 480};
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        App::UiInputAdapter adapter;
        App::OrbitCamera orbit;
        Scene::Camera camera({0, 0, 6});
        const auto sample = [&] {
            native->beginInputFrame();
            ImGui::NewFrame();
            const auto input = adapter.sample(native->input());
            orbit.apply(camera, input, {640, 480});
            ImGui::EndFrame();
            return input;
        };
        ASSERT_TRUE(io.ConfigInputTrickleEventQueue);
        position(handle, 300, 300);
        sample();
        mouse(handle, 2, GLFW_PRESS, 0);
        mouse(handle, 2, GLFW_RELEASE, 0);
        position(handle, 320, 300);
        EXPECT_TRUE(sample().pan.pressed);
        EXPECT_TRUE(sample().pan.released);
        EXPECT_EQ(camera.position(), glm::vec3(0, 0, 6));
        // 按下、释放与位移分属三批；不能按 update 轮数假定位移已经属于按住的手势。
        EXPECT_EQ(sample().delta.x, 20);
        EXPECT_EQ(camera.position(), glm::vec3(0, 0, 6));
        mouse(handle, 2, GLFW_PRESS, 0);
        ASSERT_TRUE(sample().pan.pressed);
        position(handle, 340, 300);
        EXPECT_EQ(sample().delta.x, 20);
        EXPECT_NE(camera.position(), glm::vec3(0, 0, 6));
        mouse(handle, 2, GLFW_RELEASE, 0);
        ASSERT_TRUE(sample().pan.released);

        const auto canceled = camera;
        adapter.cancel();
        orbit.cancel();
        focus(handle, GLFW_FALSE);
        focus(handle, GLFW_TRUE);
        // 四个边沿刻意排队，复现恢复时旧事件尚未消费的情况。
        for (int i = 0; i < 2; ++i)
        {
            mouse(handle, 1, GLFW_PRESS, 0);
            mouse(handle, 1, GLFW_RELEASE, 0);
        }
        for (int i = 0; i < 4; ++i)
        {
            const auto input = sample();
            EXPECT_FALSE(input.rotate.pressed);
            EXPECT_EQ(camera.position(), canceled.position());
            EXPECT_EQ(camera.rotation(), canceled.rotation());
        }
        ASSERT_TRUE(ImGui::GetCurrentContext()->InputEventsQueue.empty());
        EXPECT_FALSE(sample().rotate.down);  // 中性帧不重新启动已取消的手势。
        mouse(handle, 1, GLFW_PRESS, 0);
        ASSERT_TRUE(sample().rotate.pressed);
        position(handle, 360, 310);
        EXPECT_EQ(sample().delta.x, 20);
        EXPECT_NE(camera.rotation(), canceled.rotation());
        mouse(handle, 1, GLFW_RELEASE, 0);
        EXPECT_TRUE(sample().rotate.released);

        adapter.cancel();
        mouse(handle, 0, GLFW_PRESS, 0);
        EXPECT_FALSE(sample().select.pressed);
        EXPECT_FALSE(sample().select.down);
        mouse(handle, 0, GLFW_RELEASE, 0);
        EXPECT_FALSE(sample().select.released);
        EXPECT_FALSE(sample().select.down);
        mouse(handle, 0, GLFW_PRESS, 0);
        EXPECT_TRUE(sample().select.pressed);
        mouse(handle, 0, GLFW_RELEASE, 0);
        EXPECT_TRUE(sample().select.released);
    }

    TEST_F(EditorAppTest, ViewportDragCrossesPanelsAndReplacementRequiresFreshPress)
    {
        Editor::EditorApp session(options());
        Editor::EditorScene demo;
        int frame = -1;
        App::ViewportRegion region;
        GLFWmousebuttonfun mouse = nullptr;
        GLFWcursorposfun cursor = nullptr;
        Scene::Camera replaced;
        glm::quat moved;
        session.run({[&](auto& app) {
                         app.scene().setCamera(Scene::Camera({0, 0, 6}));
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         mouse = glfwSetMouseButtonCallback(handle, nullptr);
                         glfwSetMouseButtonCallback(handle, mouse);
                         cursor = glfwSetCursorPosCallback(handle, nullptr);
                         glfwSetCursorPosCallback(handle, cursor);
                         auto focus = glfwSetWindowFocusCallback(handle, nullptr);
                         glfwSetWindowFocusCallback(handle, focus);
                         focus(handle, GLFW_TRUE);
                     },
                     [&](auto& app, const auto&) {
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         switch (++frame)
                         {
                             case 1:
                                 cursor(handle, region.origin.x + 40, region.origin.y + 40);
                                 break;
                             case 2:
                                 mouse(handle, 1, GLFW_PRESS, 0);
                                 break;
                             case 3:
                                 cursor(handle, region.origin.x + 60, region.origin.y + 40);
                                 break;
                             case 4:
                                 EXPECT_NE(app.scene().camera().rotation(), glm::quat(1, 0, 0, 0));
                                 moved = app.scene().camera().rotation();
                                 cursor(handle, region.origin.x - 30, region.origin.y + 40);
                                 break;
                             case 5:
                                 EXPECT_NE(app.scene().camera().rotation(), moved);
                                 break;
                             case 6: {
                                 auto scene = std::make_unique<Scene::Scene>();
                                 scene->setCamera(Scene::Camera({0, 0, 8}));
                                 app.setScene(std::move(scene));
                                 replaced = app.scene().camera();
                                 break;
                             }
                             case 7:
                                 cursor(handle, region.origin.x + 40, region.origin.y + 40);
                                 break;
                             case 8:
                                 cursor(handle, region.origin.x + 80, region.origin.y + 50);
                                 break;
                             case 9:
                                 EXPECT_EQ(app.scene().camera().rotation(), replaced.rotation());
                                 mouse(handle, 1, GLFW_RELEASE, 0);
                                 break;
                             case 12:
                                 mouse(handle, 1, GLFW_PRESS, 0);
                                 break;
                             case 13:
                                 cursor(handle, region.origin.x + 100, region.origin.y + 50);
                                 break;
                             case 14:
                                 EXPECT_NE(app.scene().camera().rotation(), replaced.rotation());
                                 mouse(handle, 1, GLFW_RELEASE, 0);
                                 break;
                             case 15:
                                 app.requestExit();
                                 break;
                         }
                     },
                     {},
                     [&](auto& app) { demo.cancelInteraction(app); },
                     [&](auto& app, auto extent, const auto& input) { demo.input(app, extent, input); },
                     [&](auto& app) {
                         app.sceneViewport("Scene");
                         region = app.viewportRegion();
                     }});
        EXPECT_EQ(frame, 15);
        cleanValidation();
    }

    TEST_F(EditorAppTest, RunForwardsPanAndRestartsRotationAfterFocusCancellation)
    {
        Editor::EditorApp session(options());
        Editor::EditorScene demo;
        GLFWmousebuttonfun mouse = nullptr;
        GLFWcursorposfun position = nullptr;
        GLFWwindowfocusfun focus = nullptr;
        int frame = -1, cancellations = 0;
        Scene::Camera canceled;
        session.run({[&](auto& app) {
                         app.scene().setCamera(Scene::Camera({0, 0, 6}));
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         mouse = glfwSetMouseButtonCallback(handle, nullptr);
                         glfwSetMouseButtonCallback(handle, mouse);
                         position = glfwSetCursorPosCallback(handle, nullptr);
                         glfwSetCursorPosCallback(handle, position);
                         focus = glfwSetWindowFocusCallback(handle, nullptr);
                         glfwSetWindowFocusCallback(handle, focus);
                         focus(handle, GLFW_TRUE);
                     },
                     [&](auto& app, const auto&) {
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         switch (++frame)
                         {
                             case 0:
                                 position(handle, 500, 300);
                                 break;
                             case 1:
                                 mouse(handle, 2, GLFW_PRESS, 0);
                                 break;
                             case 2:
                                 position(handle, 520, 300);
                                 break;
                             case 3:
                                 EXPECT_NE(app.scene().camera().position(), glm::vec3(0, 0, 6));
                                 mouse(handle, 2, GLFW_RELEASE, 0);
                                 break;
                             case 4:
                                 canceled = app.scene().camera();
                                 focus(handle, GLFW_FALSE);
                                 break;
                             case 5:
                                 focus(handle, GLFW_TRUE);
                                 break;
                             case 6:
                                 // 焦点丢失会清空 ImGui 鼠标位置，先恢复基准，再开始新的拖动。
                                 position(handle, 520, 300);
                                 EXPECT_EQ(app.scene().camera().position(), canceled.position());
                                 EXPECT_EQ(app.scene().camera().rotation(), canceled.rotation());
                                 break;
                             case 7:
                                 mouse(handle, 1, GLFW_PRESS, 0);
                                 break;
                             case 8:
                                 position(handle, 540, 310);
                                 break;
                             case 9:
                                 EXPECT_NE(app.scene().camera().rotation(), canceled.rotation());
                                 mouse(handle, 1, GLFW_RELEASE, 0);
                                 break;
                             case 10:
                                 app.requestExit();
                                 break;
                         }
                     },
                     {},
                     [&](auto& app) {
                         ++cancellations;
                         demo.cancelInteraction(app);
                     },
                     [&](auto& app, auto extent, const auto& input) {
                         EXPECT_NE(frame, 4);
                         EXPECT_TRUE(ImGui::GetCurrentContext()->InputEventsQueue.empty());
                         if (frame == 6)
                             EXPECT_EQ(input.position, (Input::PointerValue{520, 300}));
                         if (frame == 1)
                             EXPECT_TRUE(input.pan.pressed);
                         if (frame == 7)
                             EXPECT_TRUE(input.rotate.pressed);
                         if (frame == 8)
                             EXPECT_EQ(input.delta, (Input::PointerValue{20, 10}));
                         demo.input(app, extent, input);
                     }});
        EXPECT_EQ(cancellations, 1);
        cleanValidation();
    }

    TEST_F(EditorAppTest, BothShowcaseModelsPresentSharedInstances)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 800;
        config.window.height = 600;
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(config, std::make_unique<TrackedFileIO>(state));
        auto demo = std::make_shared<Editor::EditorScene>();
        int frames = 0;
        std::shared_ptr<const Model::PreparedModel> gltf;
        const auto presented = [&](Editor::EditorApp& app) {
            const auto image = WindowCapture::capture(app.window());
            const auto bounds = WindowCapture::bounds(image, 370);
            EXPECT_GT(bounds[2], bounds[0]);
            EXPECT_GT(bounds[3], bounds[1]);
            const auto entities = app.scene().entities();
            ASSERT_EQ(entities.size(), 2u);
            EXPECT_NE(entities[0].model, nullptr);
            EXPECT_EQ(entities[0].model, entities[1].model);
        };
        Editor::Callbacks callbacks{[&, demo](auto& app) {
                                        demo->initialize(app);
                                        EditorTest::setSharedEditorScene(app, *demo, false);
                                        gltf = app.scene().entities()[0].model;
                                    },
                                    [&, demo](auto& app, const auto&) {
                                        if (frames == 2)
                                        {
                                            ASSERT_NO_FATAL_FAILURE(presented(app));
                                            EditorTest::setSharedEditorScene(app, *demo, true);
                                            EXPECT_NE(app.scene().entities()[0].model, gltf);
                                            track(app, app.scene().entities()[0].model, state);
                                        }
                                        if (frames++ == 4)
                                        {
                                            ASSERT_NO_FATAL_FAILURE(presented(app));
                                            app.requestExit();
                                        }
                                    },
                                    [&, demo](auto& app) {
                                        demo->ui(app);
                                        glfwWaitEventsTimeout(0.04);
                                    }};
        demo.reset();
        session.run(std::move(callbacks));
        closed(state);
        cleanValidation();
    }

    TEST_F(EditorAppTest, SceneCommandsAndUiChangesReachCurrentFrame)
    {
        copyShowcaseModels();
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
        auto demo = std::make_shared<Editor::EditorScene>();
        const std::weak_ptr<Editor::EditorScene> lifetime = demo;
        int updates = 0;
        Editor::Callbacks callbacks{[demo](auto& app) {
                                        demo->initialize(app);
                                        EditorTest::setSharedEditorScene(app, *demo, false);
                                    },
                                    [&, demo](auto& app, const auto&) {
                                        if (++updates == 3)
                                            app.requestExit();
                                    },
                                    [&, demo](auto& app) {
                                        demo->ui(app);
                                        if (updates != 2)
                                            return;
                                        const auto first = demo->selected();
                                        const auto second = app.scene().entities()[1].id;
                                        const auto gltf = app.scene().find(first)->model;
                                        const auto opens = state->opens;
                                        // 这是 UI 回调内的方法接入检查；实际控件输入另由 LightingWidgets 用例验证。
                                        ASSERT_TRUE(app.scene().setVisible(first, false));
                                        const auto frame = Scene::extractSceneFrame(app.scene(), {640, 480});
                                        ASSERT_TRUE(frame);
                                        EXPECT_EQ(frame->draws.size(), gltf->draws().size());
                                        const auto created = demo->createEntity(app);
                                        EXPECT_EQ(demo->selected(), created);
                                        EXPECT_EQ(app.scene().find(created)->model, gltf);
                                        demo->deleteSelected(app);
                                        EXPECT_FALSE(demo->selected().valid());
                                        EXPECT_EQ(app.scene().find(created), nullptr);
                                        EXPECT_NE(app.scene().find(second), nullptr);
                                        const auto lighting = app.scene().lighting();
                                        const auto camera = app.scene().camera();
                                        demo->clear(app);
                                        EXPECT_TRUE(app.scene().entities().empty());
                                        EXPECT_FALSE(demo->selected().valid());
                                        EXPECT_EQ(app.scene().lighting(), lighting);
                                        EXPECT_EQ(app.scene().camera().position(), camera.position());
                                        demo->createEntity(app);
                                        app.scene().setCamera(Scene::Camera({0, 0, 8}));
                                        EXPECT_EQ(app.scene().camera().position().z, 8);
                                        EditorTest::setSharedEditorScene(app, *demo, true);
                                        const auto obj = app.scene().find(demo->selected())->model;
                                        EXPECT_NE(obj, gltf);
                                        EXPECT_EQ(app.scene().find(first), nullptr);
                                        EXPECT_EQ(app.scene().camera().position().z, 6);
                                        EditorTest::setSharedEditorScene(app, *demo, false);
                                        EXPECT_EQ(app.scene().find(demo->selected())->model, gltf);
                                        EXPECT_EQ(state->opens, opens);
                                        track(app, gltf, state);
                                    }};
        demo.reset();
        session.run(std::move(callbacks));
        EXPECT_TRUE(lifetime.expired());
        closed(state);
        cleanValidation();
    }

    TEST_F(EditorAppTest, MixedSceneSaveLoadPreservesStateAndRenderedPixels)
    {
        copyShowcaseModels();
        std::ostringstream failures;
        Editor::EditorApp session(options());
        session.run({[&](Editor::EditorApp& app) {
                         Editor::EditorScene demo;
                         demo.initialize(app);
                         EditorTest::setSharedEditorScene(app, demo, false);

                         const auto original = app.scene().entities()[0].id;
                         const auto avocado = app.scene().find(original)->model;
                         demo.selectModel(true);
                         const auto tea = demo.createEntity(app);
                         const auto teapot = app.scene().find(tea)->model;
                         ASSERT_NE(avocado, teapot);
                         const Scene::Transform pose({1.6f, 0, 0}, {1, 0, 0, 0}, {0.075f, 0.075f, 0.075f});
                         ASSERT_TRUE(app.scene().setTransform(tea, pose));
                         app.scene().createEntity({}, Scene::Transform({5, 1, 2}, {1, 0, 0, 1}, {-1, 0, 2}), false);
                         app.scene().setLighting({{0.2f, -1, -2}, {0.7f, 0.8f, 1}, 1.2f, 0.2f});
                         app.clearColor() = {0.1f, 0.2f, 0.3f, 1};
                         const auto camera = app.scene().camera();
                         auto before = Scene::extractSceneFrame(app.scene(), {384, 256});
                         ASSERT_TRUE(before);
                         const auto pixels = TextureGpuReview::captureFrame(app.renderer(), before->input());
                         const auto uploads = count("Uploaded mesh:");
                         ASSERT_TRUE(demo.save(app, "mixed-save.json")) << demo.message();
                         EXPECT_FALSE(demo.load(app, "missing-scene.json"));
                         EXPECT_NE(app.scene().find(tea), nullptr);
                         EXPECT_EQ(demo.selected(), tea);
                         EXPECT_EQ(app.clearColor(), (std::array<float, 4>{0.1f, 0.2f, 0.3f, 1}));
                         auto invalidModel = Scene::captureScene(app.scene(), app.clearColor());
                         invalidModel.entities[2].model = Model::ModelReference("models/missing.gltf");
                         Scene::writeScene(app.fileIO(), "missing-model.json", invalidModel);
                         auto failureSink = std::make_shared<spdlog::sinks::ostream_sink_mt>(failures);
                         failureSink->set_pattern("%l|%v");
                         Log::Registry::get("resource").addSink(failureSink);
                         Log::Registry::get("editor").addSink(failureSink);
                         const std::string associatedFile(demo.scenePath());
                         EXPECT_FALSE(demo.load(app, "missing-model.json"));
                         EXPECT_EQ(demo.scenePath(), associatedFile);
                         EXPECT_NE(demo.message().find("models/missing.gltf"), std::string::npos);
                         const auto recordedFailure = failures.str();
                         const auto firstFailure = recordedFailure.find("error|");
                         ASSERT_NE(firstFailure, std::string::npos);
                         EXPECT_EQ(recordedFailure.find("error|", firstFailure + 6), std::string::npos);
                         EXPECT_NE(app.scene().find(tea), nullptr);
                         EXPECT_EQ(demo.selected(), tea);
                         EXPECT_EQ(app.scene().camera().position(), camera.position());
                         ASSERT_TRUE(demo.load(app, "mixed-save.json")) << demo.message();
                         EXPECT_FALSE(demo.selected().valid());
                         EXPECT_EQ(app.scene().find(original), nullptr);
                         ASSERT_EQ(app.scene().entities().size(), 4u);
                         EXPECT_EQ(app.scene().entities()[0].model, avocado);
                         EXPECT_EQ(app.scene().entities()[2].model, teapot);
                         EXPECT_FALSE(app.scene().entities()[3].visible);
                         EXPECT_EQ(app.scene().entities()[3].model, nullptr);
                         EXPECT_EQ(app.scene().entities()[2].transform.scale(), pose.scale());
                         EXPECT_EQ(app.scene().camera().position(), camera.position());
                         auto after = Scene::extractSceneFrame(app.scene(), {384, 256});
                         ASSERT_TRUE(after);
                         const auto restored = TextureGpuReview::captureFrame(app.renderer(), after->input());
                         ASSERT_EQ(pixels.size(), restored.size());
                         for (size_t i = 0; i < pixels.size(); ++i)
                             EXPECT_LE(std::abs(int(pixels[i]) - int(restored[i])), 2) << i;
                         const auto saveImage = [](const char* path, const std::vector<uint8_t>& rgba) {
                             std::ofstream output(path, std::ios::binary);
                             output << "P6\n384 256\n255\n";
                             for (size_t i = 0; i < rgba.size(); i += 4)
                                 output.write(reinterpret_cast<const char*>(rgba.data() + i), 3);
                             return bool(output);
                         };
                         ASSERT_TRUE(saveImage("scene-before.ppm", pixels));
                         ASSERT_TRUE(saveImage("scene-restored.ppm", restored));
                         EXPECT_EQ(count("Uploaded mesh:"), uploads);
                         ASSERT_TRUE(demo.load(app, "assets/scenes/mixed.json")) << demo.message();
                         ASSERT_EQ(app.scene().entities().size(), 2u);
                         EXPECT_EQ(app.scene().entities()[0].model, avocado);
                         EXPECT_EQ(app.scene().entities()[1].model, teapot);
                         demo.selectModel(false);
                         EXPECT_EQ(app.scene().find(demo.createEntity(app))->model, avocado);
                         demo.clear(app);
                         ASSERT_TRUE(demo.save(app, "empty-save.json"));
                         ASSERT_TRUE(demo.load(app, "empty-save.json"));
                         EXPECT_TRUE(app.scene().entities().empty());
                         app.requestExit();
                     },
                     {},
                     {}});
        cleanValidation();
    }

    TEST_F(EditorAppTest, SceneFileCommandsObserveSameUiFrame)
    {
        copyShowcaseModels();
        Editor::EditorApp session(options());
        auto demo = std::make_shared<Editor::EditorScene>();
        session.run({[demo](auto& app) {
            demo->initialize(app);
            EditorTest::setSharedEditorScene(app, *demo, false);
        }, {}, [demo](auto& app) {
            demo->ui(app);
            const auto old = demo->selected();
            const Scene::Transform edited({2, 1, 0}, {1, 0, 0, 0}, {-2, 0, 3});
            ASSERT_TRUE(app.scene().setTransform(old, edited));
            ASSERT_TRUE(demo->save(app, "same-frame.json"));
            app.scene().setTransform(old, Scene::Transform{});
            ASSERT_TRUE(demo->load(app, "same-frame.json"));
            EXPECT_EQ(app.scene().find(old), nullptr);
            EXPECT_FALSE(demo->selected().valid());
            ASSERT_EQ(app.scene().entities().size(), 2u);
            EXPECT_EQ(app.scene().entities()[0].transform.matrix(), edited.matrix());
            const auto frame = Scene::extractSceneFrame(app.scene(), {640, 480}, false);
            ASSERT_TRUE(frame);
            EXPECT_EQ(frame->draws[0].model, edited.matrix() * app.scene().entities()[0].model->draws()[0].model);
            app.requestExit();
        }});
        demo.reset();
        cleanValidation();
    }

    struct CallbackHold
    {
        explicit CallbackHold(std::shared_ptr<Lifetime> state) : state(std::move(state))
        {
            this->state->callbackReleased = false;
        }
        ~CallbackHold()
        {
            if (cpu.valid())
                ResourceManager::instance().release(cpu);
            EXPECT_TRUE(ResourceManager::instance().hasLoader(ResourceType::MODEL));
            EXPECT_TRUE(state->mesh.valid());
            state->callbackReleased = true;
        }
        std::shared_ptr<Lifetime> state;
        ResourceId cpu;
    };

    TEST_F(EditorAppTest, InitializeUpdateAndUiExceptionsReleaseCallbacksAndResourcesBeforeFixtureCleanup)
    {
        for (int phase = 0; phase < 3; ++phase)
        {
            SCOPED_TRACE(phase);
            auto state = std::make_shared<Lifetime>();
            Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
            auto hold = std::make_shared<CallbackHold>(state);
            int updates = 0, uis = 0;
            Editor::Callbacks callbacks{[&, hold](Editor::EditorApp& app) {
                                            auto model = Model::prepareModel(app.renderer(), GLTF);
                                            track(app, model, state);
                                            app.scene().createEntity(model);
                                            hold->cpu = ResourceRegistry::instance().lookup(ResourceType::TEXTURE, PNG);
                                            ResourceManager::instance().acquire(hold->cpu);
                                            EXPECT_NE(ResourceManager::instance().get<TextureResource>(hold->cpu),
                                                      nullptr);
                                            if (phase == 0)
                                                throw std::runtime_error("initialize failure");
                                        },
                                        [&, hold](Editor::EditorApp&, const Editor::FrameTime&) {
                                            ++updates;
                                            if (phase == 1)
                                                throw std::runtime_error("update failure");
                                        },
                                        [&, hold](Editor::EditorApp&) {
                                            ++uis;
                                            throw std::runtime_error("ui failure");
                                        }};
            hold.reset();
            EXPECT_THROW(session.run(std::move(callbacks)), std::runtime_error);
            EXPECT_FALSE(session.isOpen());
            EXPECT_EQ(updates, phase == 0 ? 0 : 1);
            EXPECT_EQ(uis, phase == 2 ? 1 : 0);
            closed(state);
            unloaded(ResourceType::MODEL, GLTF);
            unloaded(ResourceType::TEXTURE, PNG);
            unloaded(ResourceType::SHADER, "shaders/cube.vert.spv");
            unloaded(ResourceType::SHADER, "shaders/cube.frag.spv");
        }
        cleanValidation();
    }

    TEST_F(EditorAppTest, MissingModelPngStopsEditorAppAndReleasesDependencyHolds)
    {
        fs::remove("assets/textures/direction_rgba.png");
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
        int updates = 0;
        EXPECT_THROW(session.run({[&](Editor::EditorApp& app) {
                                      Model::prepareModel(app.renderer(), GLTF);
                                      app.requestExit();
                                  },
                                  [&](Editor::EditorApp&, const Editor::FrameTime&) { ++updates; },
                                  {}}),
                     Error::Exception);
        EXPECT_EQ(updates, 0);
        EXPECT_FALSE(session.isOpen());
        closed(state);
        unloaded(ResourceType::MODEL, GLTF);
        unloaded(ResourceType::TEXTURE, PNG);
        EXPECT_EQ(count("Uploaded mesh:"), 0u);
        cleanValidation();
    }

    TEST_F(EditorAppTest, ExitWindowCloseAndDestructionStopFurtherCallbacks)
    {
        for (int mode = 0; mode < 4; ++mode)
        {
            SCOPED_TRACE(mode);
            auto state = std::make_shared<Lifetime>();
            int updates = 0, uis = 0;
            {
                Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
                auto model = Model::prepareModel(session.renderer(), GLTF);
                track(session, model, state);
                session.scene().createEntity(model);
                model.reset();
                if (mode < 3)
                    session.run({{},
                                 [&](Editor::EditorApp& app, const Editor::FrameTime&) {
                                     if (++updates == 3 && mode < 2)
                                     {
                                         if (mode == 0)
                                             app.requestExit();
                                         else
                                             app.window().setShouldClose(true);
                                     }
                                 },
                                 [&](Editor::EditorApp& app) {
                                     ++uis;
                                     if (mode == 2)
                                         app.requestExit();
                                 }});
                else
                    session.requestExit();  // 未调用 run 就销毁对象也是正常的关闭路径。
            }
            EXPECT_EQ(updates, mode < 2 ? 3 : mode == 2 ? 1 : 0);
            EXPECT_EQ(uis, mode < 2 ? 2 : mode == 2 ? 1 : 0);
            closed(state);
        }
        cleanValidation();
    }

    TEST_F(EditorAppTest, ContinuousPropertyEditsAndColorPickersMergeIntoSingleUndoStepAndCancelRollback)
    {
        copyShowcaseModels();
        auto opts = options();
        opts.window.width = 1280;
        opts.window.height = 800;
        Editor::EditorApp session(opts);
        Editor::EditorScene editor;
        GLFWmousebuttonfun mouse = nullptr;
        GLFWcursorposfun cursor = nullptr;
        GLFWwindowfocusfun focus = nullptr;

        Scene::EntityId targetId;
        std::string initialName;
        glm::vec3 initialPos{};
        Render::Lighting initialLighting{};
        std::array<float, 4> initialClearColor{};

        enum class Phase
        {
            NameActivate,
            NameType1,
            NameType2,
            NameCommit,
            NameReleaseKey,
            NameVerifyAndNoOpActivate,
            NameNoOpCommit,
            NameNoOpReleaseKey,
            FindPosition,
            PositionPress,
            PositionDrag1,
            PositionDrag2,
            PositionRelease,
            PositionVerify,
            FindBg,
            BgPress,
            BgReleaseToOpenPicker,
            BgHoverSv,
            BgPressSv,
            BgDragSv,
            BgReleaseSv,
            BgClosePickerPress,
            BgClosePickerRelease,
            BgVerify,
            FindLightColor,
            LightColorPress,
            LightColorReleaseToOpenPicker,
            LightColorHoverSv,
            LightColorPressSv,
            LightColorDragSv,
            LightColorReleaseSv,
            LightColorClosePickerPress,
            LightColorClosePickerRelease,
            LightColorVerify,
            FindIntensity,
            IntensityPress,
            IntensityDrag,
            IntensityFocusLost,
            IntensityRestoreFocus,
            IntensityVerifyCancelled,
            Done
        };
        Phase phase = Phase::NameActivate;
        ImGuiID queuedActivateId = 0;
        float scanY = 0.0f;
        double foundX = 0.0;
        double foundY = 0.0;
        int waitFrames = 0;
        int totalFrames = 0;
        bool failed = false;

        auto propsWindow = []() { return ImGui::FindWindowByName("Properties"); };
        auto itemId = [](ImGuiWindow* win, const char* idStr, const char* child) -> ImGuiID {
            const ImGuiID base = idStr ? win->GetID(idStr) : win->ID;
            return ImHashStr(child, 0, base);
        };
        auto zeroSubItemId = [](ImGuiWindow* win, const char* parent) -> ImGuiID {
            const int zero = 0;
            return ImHashStr("", 0, ImHashData(&zero, sizeof(zero), win->GetID(parent)));
        };
        auto scanItem = [&](GLFWwindow* handle, ImGuiWindow* props, ImGuiID target, Phase nextPhase) {
            if (ImGui::GetHoveredID() == target)
            {
                foundX = ImGui::GetIO().MousePos.x;
                foundY = ImGui::GetIO().MousePos.y;
                scanY = 0.0f;
                phase = nextPhase;
                return;
            }
            const float minY = props->InnerRect.Min.y + 8.0f;
            const float maxY = props->InnerRect.Max.y - 8.0f;
            if (scanY < minY)
                scanY = minY;
            else
                scanY += 12.0f;
            if (scanY > maxY)
            {
                scanY = minY;
                props->Scroll.y = props->Scroll.y >= props->ScrollMax.y
                                      ? 0.0f
                                      : std::min(props->Scroll.y + 120.0f, props->ScrollMax.y);
            }
            cursor(handle, static_cast<double>(props->WorkRect.Min.x + 10.0f), static_cast<double>(scanY));
        };

        session.run(
            {[&](auto& app) {
                 editor.initialize(app, "assets/scenes/shadows.json");
                 ASSERT_FALSE(app.scene().entities().empty());
                 targetId = app.scene().entities().front().id;
                 editor.select(targetId);
                 initialName = app.scene().find(targetId)->name;
                 initialPos = app.scene().find(targetId)->transform.position();
                 initialLighting = app.scene().lighting();
                 initialClearColor = app.clearColor();

                 auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                 mouse = glfwSetMouseButtonCallback(handle, nullptr);
                 glfwSetMouseButtonCallback(handle, mouse);
                 cursor = glfwSetCursorPosCallback(handle, nullptr);
                 glfwSetCursorPosCallback(handle, cursor);
                 focus = glfwSetWindowFocusCallback(handle, nullptr);
                 glfwSetWindowFocusCallback(handle, focus);
                 focus(handle, GLFW_TRUE);
             },
             [&](auto& app, const auto&) {
                 if (phase == Phase::Done || failed)
                     return;
                 if (++totalFrames > 1500)
                 {
                     failed = true;
                     ADD_FAILURE() << "Timeout at phase " << static_cast<int>(phase)
                                   << " hovered=" << ImGui::GetHoveredID()
                                   << " active=" << ImGui::GetActiveID()
                                   << " mouse=(" << ImGui::GetIO().MousePos.x << "," << ImGui::GetIO().MousePos.y
                                   << ") down0=" << ImGui::GetIO().MouseDown[0];
                     app.requestExit();
                     return;
                 }
                 if (waitFrames > 0)
                 {
                     --waitFrames;
                     return;
                 }
                 auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                 auto* props = propsWindow();
                 if (!props)
                     return;
                 const std::string idStr = std::to_string(targetId.value());

                 switch (phase)
                 {
                     case Phase::NameActivate:
                         queuedActivateId = itemId(props, idStr.c_str(), "##entity-name");
                         phase = Phase::NameType1;
                         break;
                     case Phase::NameType1:
                         ImGui::GetIO().AddInputCharactersUTF8("_A");
                         phase = Phase::NameType2;
                         break;
                     case Phase::NameType2:
                         ImGui::GetIO().AddInputCharactersUTF8("B");
                         phase = Phase::NameCommit;
                         break;
                     case Phase::NameCommit:
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
                         phase = Phase::NameReleaseKey;
                         break;
                     case Phase::NameReleaseKey:
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
                         phase = Phase::NameVerifyAndNoOpActivate;
                         break;
                     case Phase::NameVerifyAndNoOpActivate:
                         EXPECT_TRUE(editor.canUndo());
                         EXPECT_NE(app.scene().find(targetId)->name, initialName);
                         ASSERT_TRUE(editor.undo(app));
                         EXPECT_EQ(app.scene().find(targetId)->name, initialName);
                         EXPECT_FALSE(editor.canUndo());
                         // 仅聚焦 Name 再离开而不改动内容：不应产生撤销记录。
                         queuedActivateId = itemId(props, idStr.c_str(), "##entity-name");
                         phase = Phase::NameNoOpCommit;
                         break;
                     case Phase::NameNoOpCommit:
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
                         phase = Phase::NameNoOpReleaseKey;
                         break;
                     case Phase::NameNoOpReleaseKey:
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
                         phase = Phase::FindPosition;
                         break;
                     case Phase::FindPosition:
                         EXPECT_FALSE(editor.canUndo());
                         scanItem(handle, props, zeroSubItemId(props, "##position"), Phase::PositionPress);
                         break;
                     case Phase::PositionPress:
                         mouse(handle, 0, GLFW_PRESS, 0);
                         phase = Phase::PositionDrag1;
                         break;
                     case Phase::PositionDrag1:
                         cursor(handle, foundX + 30.0, foundY);
                         phase = Phase::PositionDrag2;
                         break;
                     case Phase::PositionDrag2:
                         cursor(handle, foundX + 70.0, foundY);
                         EXPECT_FALSE(editor.canUndo());
                         phase = Phase::PositionRelease;
                         break;
                     case Phase::PositionRelease:
                         mouse(handle, 0, GLFW_RELEASE, 0);
                         phase = Phase::PositionVerify;
                         break;
                     case Phase::PositionVerify:
                         if (app.scene().find(targetId)->transform.position() == initialPos || !editor.undo(app))
                         {
                             failed = true;
                             ADD_FAILURE() << "Position drag/undo failed";
                             app.requestExit();
                             return;
                         }
                         EXPECT_EQ(app.scene().find(targetId)->transform.position(), initialPos);
                         EXPECT_FALSE(editor.canUndo());
                         props->Scroll.y = props->ScrollMax.y;
                         phase = Phase::FindBg;
                         break;
                     case Phase::FindBg:
                         scanItem(handle, props, itemId(props, "##background", "##ColorButton"), Phase::BgPress);
                         break;
                     case Phase::BgPress:
                         mouse(handle, 0, GLFW_PRESS, 0);
                         phase = Phase::BgReleaseToOpenPicker;
                         break;
                     case Phase::BgReleaseToOpenPicker:
                         mouse(handle, 0, GLFW_RELEASE, 0);
                         phase = Phase::BgHoverSv;
                         break;
                     case Phase::BgHoverSv:
                         if (auto* g = ImGui::GetCurrentContext(); !g->OpenPopupStack.empty())
                         {
                             const auto* win = g->OpenPopupStack.back().Window;
                             if (win->Hidden)
                                 break;
                             foundX = win->InnerRect.Min.x + 50.0;
                             foundY = win->InnerRect.Min.y + 50.0;
                             cursor(handle, foundX, foundY);
                             phase = Phase::BgPressSv;
                         }
                         else
                         {
                             failed = true;
                             ADD_FAILURE() << "Background picker popup did not open";
                             app.requestExit();
                         }
                         break;
                     case Phase::BgPressSv:
                         mouse(handle, 0, GLFW_PRESS, 0);
                         phase = Phase::BgDragSv;
                         break;
                     case Phase::BgDragSv:
                         cursor(handle, foundX + 60.0, foundY + 50.0);
                         EXPECT_FALSE(editor.canUndo());
                         phase = Phase::BgReleaseSv;
                         break;
                     case Phase::BgReleaseSv:
                         mouse(handle, 0, GLFW_RELEASE, 0);
                         phase = Phase::BgClosePickerPress;
                         break;
                     case Phase::BgClosePickerPress:
                         cursor(handle, 10.0, 10.0);
                         mouse(handle, 0, GLFW_PRESS, 0);
                         phase = Phase::BgClosePickerRelease;
                         break;
                     case Phase::BgClosePickerRelease:
                         mouse(handle, 0, GLFW_RELEASE, 0);
                         phase = Phase::BgVerify;
                         break;
                     case Phase::BgVerify:
                         if (app.clearColor() == initialClearColor || !editor.undo(app))
                         {
                             const auto* g = ImGui::GetCurrentContext();
                             failed = true;
                             ADD_FAILURE() << "Background picker drag/undo failed; clear=(" << app.clearColor()[0]
                                           << "," << app.clearColor()[1] << "," << app.clearColor()[2]
                                           << ") canUndo=" << editor.canUndo()
                                           << " popups=" << g->OpenPopupStack.size()
                                           << " found=(" << foundX << "," << foundY << ")";
                             app.requestExit();
                             return;
                         }
                         EXPECT_EQ(app.clearColor(), initialClearColor);
                         EXPECT_FALSE(editor.canUndo());
                         phase = Phase::FindLightColor;
                         break;
                     case Phase::FindLightColor:
                         scanItem(handle, props, itemId(props, "##lightColor", "##ColorButton"),
                                  Phase::LightColorPress);
                         break;
                     case Phase::LightColorPress:
                         mouse(handle, 0, GLFW_PRESS, 0);
                         phase = Phase::LightColorReleaseToOpenPicker;
                         break;
                     case Phase::LightColorReleaseToOpenPicker:
                         mouse(handle, 0, GLFW_RELEASE, 0);
                         phase = Phase::LightColorHoverSv;
                         break;
                     case Phase::LightColorHoverSv:
                         if (auto* g = ImGui::GetCurrentContext(); !g->OpenPopupStack.empty())
                         {
                             const auto* win = g->OpenPopupStack.back().Window;
                             if (win->Hidden)
                                 break;
                             foundX = win->InnerRect.Min.x + 50.0;
                             foundY = win->InnerRect.Min.y + 50.0;
                             cursor(handle, foundX, foundY);
                             phase = Phase::LightColorPressSv;
                         }
                         else
                         {
                             failed = true;
                             ADD_FAILURE() << "LightColor picker popup did not open";
                             app.requestExit();
                         }
                         break;
                     case Phase::LightColorPressSv:
                         mouse(handle, 0, GLFW_PRESS, 0);
                         phase = Phase::LightColorDragSv;
                         break;
                     case Phase::LightColorDragSv:
                         cursor(handle, foundX + 60.0, foundY + 50.0);
                         EXPECT_FALSE(editor.canUndo());
                         phase = Phase::LightColorReleaseSv;
                         break;
                     case Phase::LightColorReleaseSv:
                         mouse(handle, 0, GLFW_RELEASE, 0);
                         phase = Phase::LightColorClosePickerPress;
                         break;
                     case Phase::LightColorClosePickerPress:
                         cursor(handle, 10.0, 10.0);
                         mouse(handle, 0, GLFW_PRESS, 0);
                         phase = Phase::LightColorClosePickerRelease;
                         break;
                     case Phase::LightColorClosePickerRelease:
                         mouse(handle, 0, GLFW_RELEASE, 0);
                         phase = Phase::LightColorVerify;
                         break;
                     case Phase::LightColorVerify:
                         if (app.scene().lighting().color == initialLighting.color || !editor.undo(app))
                         {
                             failed = true;
                             ADD_FAILURE() << "LightColor picker drag/undo failed";
                             app.requestExit();
                             return;
                         }
                         EXPECT_EQ(app.scene().lighting().color, initialLighting.color);
                         EXPECT_FALSE(editor.canUndo());
                         phase = Phase::FindIntensity;
                         break;
                     case Phase::FindIntensity:
                         scanItem(handle, props, props->GetID("##intensity"), Phase::IntensityPress);
                         break;
                     case Phase::IntensityPress:
                         mouse(handle, 0, GLFW_PRESS, 0);
                         phase = Phase::IntensityDrag;
                         break;
                     case Phase::IntensityDrag:
                         cursor(handle, static_cast<double>(props->WorkRect.Min.x + 120.0f), foundY);
                         phase = Phase::IntensityFocusLost;
                         break;
                     case Phase::IntensityFocusLost:
                         EXPECT_NE(app.scene().lighting().intensity, initialLighting.intensity);
                         focus(handle, GLFW_FALSE);
                         phase = Phase::IntensityRestoreFocus;
                         break;
                     case Phase::IntensityRestoreFocus:
                         mouse(handle, 0, GLFW_RELEASE, 0);
                         focus(handle, GLFW_TRUE);
                         phase = Phase::IntensityVerifyCancelled;
                         break;
                     case Phase::IntensityVerifyCancelled:
                         EXPECT_FLOAT_EQ(app.scene().lighting().intensity, initialLighting.intensity);
                         EXPECT_FALSE(editor.canUndo());
                         phase = Phase::Done;
                         app.requestExit();
                         break;
                     case Phase::Done:
                         break;
                 }
             },
             [&](auto& app) {
                 if (queuedActivateId != 0)
                 {
                     ImGui::ActivateItemByID(queuedActivateId);
                     queuedActivateId = 0;
                 }
                 editor.ui(app);
             },
             [&](auto& app) { editor.cancelInteraction(app); },
             [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
             [&](auto& app) { editor.layout(app); }, [&](auto& app) { editor.requestClose(app); }});
        EXPECT_EQ(phase, Phase::Done);
        cleanValidation();
    }

    TEST_F(EditorAppTest, EditMenuAndWindowShortcutsUndoRedoWorkWithoutViewportFocus)
    {
        copyShowcaseModels();
        auto opts = options();
        opts.window.width = 1280;
        opts.window.height = 800;
        Editor::EditorApp session(opts);
        Editor::EditorScene editor;
        GLFWwindowfocusfun focus = nullptr;

        Scene::EntityId targetId;
        glm::vec3 initialPos{};
        int step = 0;
        int totalFrames = 0;
        bool done = false;

        auto injectCombo = [](ImGuiKey key, bool down, bool shift = false) {
            auto& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, down);
            io.AddKeyEvent(ImGuiKey_LeftCtrl, down);
            if (shift)
            {
                io.AddKeyEvent(ImGuiMod_Shift, down);
                io.AddKeyEvent(ImGuiKey_LeftShift, down);
            }
            io.AddKeyEvent(key, down);
        };
        auto clickPopupItem = [](const char* label) {
            auto* g = ImGui::GetCurrentContext();
            if (!g || g->OpenPopupStack.empty() || !g->OpenPopupStack.back().Window)
                return false;
            ImGui::ActivateItemByID(g->OpenPopupStack.back().Window->GetID(label));
            return true;
        };
        auto menuButtonId = [](ImGuiWindow* bar, const char* label) -> ImGuiID {
            return ImHashStr("", 0, ImHashStr(label, 0, bar->GetID("##MenuBar")));
        };

        session.run(
            {[&](auto& app) {
                 editor.initialize(app, "assets/scenes/shadows.json");
                 ASSERT_FALSE(app.scene().entities().empty());
                 targetId = app.scene().entities().front().id;
                 editor.select(targetId);
                 initialPos = app.scene().find(targetId)->transform.position();

                 auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                 focus = glfwSetWindowFocusCallback(handle, nullptr);
                 glfwSetWindowFocusCallback(handle, focus);
                 focus(handle, GLFW_TRUE);
             },
             [&](auto& app, const auto&) {
                 if (done)
                     return;
                 if (++totalFrames > 800)
                 {
                     done = true;
                     ADD_FAILURE() << "Shortcut test timeout at step " << step;
                     app.requestExit();
                     return;
                 }
                 auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                 auto* bar = ImGui::FindWindowByName("##MainMenuBar");
                 auto* props = ImGui::FindWindowByName("Properties");
                 if (!bar || !props)
                     return;
                 const std::string idStr = std::to_string(targetId.value());

                 switch (step)
                 {
                     case 0:  // 激活顶部 View 菜单（在 update 中设置，同帧 NewFrame + layout 生效）
                         ImGui::ActivateItemByID(menuButtonId(bar, "View"));
                         ++step;
                         break;
                     case 1:
                         if (clickPopupItem("Scene viewport"))
                             ++step;
                         break;
                     case 2:
                         if (app.sceneExtent().width == 0 && ImGui::GetCurrentContext()->OpenPopupStack.empty())
                             ++step;
                         break;
                     case 3:  // 激活 Name 并输入一次有效改动
                         ImGui::ActivateItemByID(ImHashStr("##entity-name", 0, props->GetID(idStr.c_str())));
                         ++step;
                         break;
                     case 4:
                         ImGui::GetIO().AddInputCharactersUTF8("X");
                         ++step;
                         break;
                     case 5:
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
                         ++step;
                         break;
                     case 6:
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
                         ++step;
                         break;
                     case 7:  // 在 Objects 面板聚焦且 Scene 视口已关闭时按 Ctrl+Z 撤销
                         ImGui::SetWindowFocus("Objects");
                         EXPECT_TRUE(editor.canUndo());
                         injectCombo(ImGuiKey_Z, true);
                         ++step;
                         break;
                     case 8:
                         injectCombo(ImGuiKey_Z, false);
                         ++step;
                         break;
                     case 9:  // 验证已撤销，按 Ctrl+Shift+Z 重做
                         EXPECT_FALSE(editor.canUndo());
                         EXPECT_TRUE(editor.canRedo());
                         injectCombo(ImGuiKey_Z, true, true);
                         ++step;
                         break;
                     case 10:
                         injectCombo(ImGuiKey_Z, false, true);
                         ++step;
                         break;
                     case 11:  // 验证重做生效，再 Ctrl+Z 然后 Ctrl+Y
                         EXPECT_TRUE(editor.canUndo());
                         EXPECT_FALSE(editor.canRedo());
                         injectCombo(ImGuiKey_Z, true);
                         ++step;
                         break;
                     case 12:
                         injectCombo(ImGuiKey_Z, false);
                         ++step;
                         break;
                     case 13:
                         EXPECT_FALSE(editor.canUndo());
                         injectCombo(ImGuiKey_Y, true);
                         ++step;
                         break;
                     case 14:
                         injectCombo(ImGuiKey_Y, false);
                         ++step;
                         break;
                     case 15:  // 处于 Name 输入框（WantTextInput）时 Ctrl+Z 不触发编辑器撤销
                         EXPECT_TRUE(editor.canUndo());
                         ImGui::ActivateItemByID(ImHashStr("##entity-name", 0, props->GetID(idStr.c_str())));
                         ++step;
                         break;
                     case 16:
                         injectCombo(ImGuiKey_Z, true);
                         ++step;
                         break;
                     case 17:
                         injectCombo(ImGuiKey_Z, false);
                         ++step;
                         break;
                     case 18:
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
                         ++step;
                         break;
                     case 19:
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
                         ++step;
                         break;
                     case 20:
                         EXPECT_TRUE(editor.canUndo());
                         // 弹出 Unsaved changes 确认框时 Ctrl+Z 不触发
                         editor.requestOpen(app);
                         ++step;
                         break;
                     case 21:
                         injectCombo(ImGuiKey_Z, true);
                         ++step;
                         break;
                     case 22:
                         injectCombo(ImGuiKey_Z, false);
                         ++step;
                         break;
                     case 23:
                         EXPECT_TRUE(editor.canUndo());
                         if (auto* unsaved = ImGui::FindWindowByName("Unsaved changes"))
                             ImGui::ActivateItemByID(unsaved->GetID("Cancel"));
                         ++step;
                         break;
                     case 24:  // 按住 Ctrl+Z 失焦再回焦：未松开 Z 前不触发撤销
                         injectCombo(ImGuiKey_Z, true);
                         focus(handle, GLFW_FALSE);
                         ++step;
                         break;
                     case 25:
                         focus(handle, GLFW_TRUE);
                         ++step;
                         break;
                     case 26:
                         EXPECT_TRUE(editor.canUndo());
                         injectCombo(ImGuiKey_Z, false);
                         ++step;
                         break;
                     case 27:  // 顶部 Edit 菜单点击 Undo / Redo / Duplicate / Delete
                         ImGui::ActivateItemByID(menuButtonId(bar, "Edit"));
                         ++step;
                         break;
                     case 28:
                         if (clickPopupItem("Undo"))
                             ++step;
                         break;
                     case 29:
                         EXPECT_FALSE(editor.canUndo());
                         EXPECT_TRUE(editor.canRedo());
                         ImGui::ActivateItemByID(menuButtonId(bar, "Edit"));
                         ++step;
                         break;
                     case 30:
                         if (clickPopupItem("Redo"))
                             ++step;
                         break;
                     case 31:
                         EXPECT_TRUE(editor.canUndo());
                         ImGui::ActivateItemByID(menuButtonId(bar, "Edit"));
                         ++step;
                         break;
                     case 32:
                         if (clickPopupItem("Duplicate"))
                             ++step;
                         break;
                     case 33:
                         EXPECT_EQ(app.scene().entities().size(), 4u);
                         EXPECT_EQ(app.scene().find(targetId)->transform.position(), initialPos);
                         ImGui::ActivateItemByID(menuButtonId(bar, "Edit"));
                         ++step;
                         break;
                     case 34:
                         if (clickPopupItem("Delete"))
                             ++step;
                         break;
                     case 35:
                         EXPECT_EQ(app.scene().entities().size(), 3u);
                         done = true;
                         app.requestExit();
                         break;
                 }
             },
             [&](auto& app) { editor.ui(app); }, [&](auto& app) { editor.cancelInteraction(app); },
             [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
             [&](auto& app) { editor.layout(app); }, [&](auto& app) { editor.requestClose(app); }});
        EXPECT_TRUE(done);
        cleanValidation();
    }

    TEST_F(EditorAppTest, PendingPropertyTransactionTriggersUnsavedGuardOnCloseOpenAndClear)
    {
        copyShowcaseModels();
        auto opts = options();
        opts.window.width = 1280;
        opts.window.height = 800;
        Editor::EditorApp session(opts);
        Editor::EditorScene editor;

        Scene::EntityId targetId;
        std::string savedName;
        int step = 0;
        int totalFrames = 0;
        bool done = false;

        auto activateUnsavedCancel = []() {
            if (auto* unsaved = ImGui::FindWindowByName("Unsaved changes"))
            {
                ImGui::ActivateItemByID(unsaved->GetID("Cancel"));
                return true;
            }
            return false;
        };

        session.run(
            {[&](auto& app) {
                 editor.initialize(app, "assets/scenes/mixed.json");
                 ASSERT_FALSE(app.scene().entities().empty());
                 targetId = app.scene().entities().front().id;
                 editor.select(targetId);
                 savedName = app.scene().find(targetId)->name;
                 EXPECT_FALSE(editor.isDirty());
             },
             [&](auto& app, const auto&) {
                 if (++totalFrames > 160)
                 {
                     ADD_FAILURE() << "PendingPropertyTransactionTriggersUnsavedGuardOnCloseOpenAndClear 超时 step="
                                   << step;
                     app.requestExit();
                     return;
                 }
                 auto* props = ImGui::FindWindowByName("Properties");
                 if (!props)
                     return;
                 const std::string idStr = std::to_string(targetId.value());
                 const ImGuiID nameId = ImHashStr("##entity-name", 0, props->GetID(idStr.c_str()));
                 auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());

                 switch (step)
                 {
                     case 0:
                         ImGui::ActivateItemByID(nameId);
                         ++step;
                         break;
                     case 1:
                         ImGui::GetIO().AddInputCharacter('X');
                         ++step;
                         break;
                     case 2:
                         // 仍在输入框编辑（未离开输入框）时投递 WM_CLOSE 模拟点击窗口右上角 X
                         EXPECT_NE(app.scene().find(targetId)->name, savedName);
                         PostMessageW(glfwGetWin32Window(handle), WM_CLOSE, 0, 0);
                         ++step;
                         break;
                     case 3:
                         if (activateUnsavedCancel())
                         {
                             EXPECT_FALSE(app.window().shouldClose());
                             EXPECT_TRUE(editor.isDirty());
                             EXPECT_TRUE(editor.canUndo());
                             EXPECT_NE(app.scene().find(targetId)->name, savedName);
                             ++step;
                         }
                         break;
                     case 4:
                         ASSERT_TRUE(editor.save(app, "pending-guard.json"));
                         savedName = app.scene().find(targetId)->name;
                         EXPECT_FALSE(editor.isDirty());
                         ImGui::ActivateItemByID(nameId);
                         ++step;
                         break;
                     case 5:
                         ImGui::GetIO().AddInputCharacter('Y');
                         ++step;
                         break;
                     case 6:
                         EXPECT_NE(app.scene().find(targetId)->name, savedName);
                         editor.requestOpen(app);
                         EXPECT_TRUE(editor.isDirty());
                         ++step;
                         break;
                     case 7:
                         if (activateUnsavedCancel())
                             ++step;
                         break;
                     case 8:
                         ASSERT_TRUE(editor.save(app, "pending-guard.json"));
                         savedName = app.scene().find(targetId)->name;
                         EXPECT_FALSE(editor.isDirty());
                         ImGui::ActivateItemByID(nameId);
                         ++step;
                         break;
                     case 9:
                         ImGui::GetIO().AddInputCharacter('Z');
                         ++step;
                         break;
                     case 10:
                         EXPECT_NE(app.scene().find(targetId)->name, savedName);
                         editor.requestClear(app);
                         EXPECT_TRUE(editor.isDirty());
                         EXPECT_FALSE(app.scene().entities().empty());
                         ++step;
                         break;
                     case 11:
                         if (activateUnsavedCancel())
                             ++step;
                         break;
                     case 12:
                         ASSERT_TRUE(editor.save(app, "pending-guard.json"));
                         EXPECT_FALSE(editor.isDirty());
                         done = true;
                         app.requestExit();
                         break;
                 }
             },
             [&](auto& app) { editor.ui(app); }, [&](auto& app) { editor.cancelInteraction(app); },
             [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
             [&](auto& app) { editor.layout(app); }, [&](auto& app) { editor.requestClose(app); }});
        EXPECT_TRUE(done);
        std::filesystem::remove("pending-guard.json");
        cleanValidation();
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
            {
                if (seg.axis == axis)
                    return seg.segment.end;
            }
            return {};
        };

        // 1. Translate 模式：三轴独立平移、跨出视口射线与视线平行轴退化保护
        {
            Scene::Scene scene;
            scene.setCamera(Scene::Camera({2.5f, 2.0f, 6.0f}));
            const auto id = scene.createEntity(
                nullptr, Scene::Transform({0.0f, 0.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {1.0f, 2.0f, 3.0f}),
                true, "GizmoTarget");
            const auto* entity = scene.find(id);
            ASSERT_NE(entity, nullptr);

            const auto gizmo =
                Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, region);
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

            // 跨出视口边界的外推坐标拖拽仍能正常推进平移
            const glm::vec2 xStart = axisTip(gizmo, Editor::GizmoAxis::X);
            auto outDrag = Editor::beginGizmoDrag(id, Editor::GizmoMode::Translate, Editor::GizmoAxis::X,
                                                  entity->transform, scene.camera(), extent, region, xStart);
            ASSERT_TRUE(outDrag.active);
            const auto outUpdated =
                Editor::updateGizmoDrag(outDrag, scene.camera(), extent, region, glm::vec2(1200.0f, xStart.y));
            EXPECT_TRUE(isFiniteTransform(outUpdated));
            EXPECT_GT(outUpdated.position().x, 1.0f);

            // 视线与平移轴近似平行时不产生 NaN / Inf
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

        // 2. Rotate 模式：平面分支精确转角与掠射切线分支全程锁定不跳变
        {
            Scene::Scene scene;
            scene.setCamera(Scene::Camera({0.0f, 0.0f, 6.0f}));
            const auto id = scene.createEntity(nullptr, Scene::Transform{}, true, "RotateTarget");
            const auto* entity = scene.find(id);
            ASSERT_NE(entity, nullptr);

            const auto gizmo = Editor::projectGizmo(scene, id, Editor::GizmoMode::Rotate, extent, region);
            ASSERT_TRUE(gizmo.visible);

            // 相机正对 Z 轴平面环：从 +X 环点拖向 +Y 环点（世界 +Y 对应 Vulkan 屏幕上方），应围绕 +Z 逆时针旋转约 +90 度
            const glm::dmat4 vp =
                glm::dmat4(scene.camera().matrices(extent).projection) * glm::dmat4(scene.camera().matrices(extent).view);
            auto projectWorldPoint = [&](const glm::dvec3& p) -> glm::vec2 {
                const glm::dvec4 c = vp * glm::dvec4(p, 1.0);
                return glm::vec2(region.origin.x + (c.x / c.w + 1.0) * 0.5 * region.size.x,
                                 region.origin.y + (c.y / c.w + 1.0) * 0.5 * region.size.y);
            };
            const glm::vec2 ringPlusX = projectWorldPoint({gizmo.worldLength, 0.0, 0.0});
            const glm::vec2 ringPlusY = projectWorldPoint({0.0, gizmo.worldLength, 0.0});
            auto planarDrag = Editor::beginGizmoDrag(id, Editor::GizmoMode::Rotate, Editor::GizmoAxis::Z,
                                                     entity->transform, scene.camera(), extent, region, ringPlusX);
            ASSERT_TRUE(planarDrag.active);
            EXPECT_TRUE(planarDrag.planarMode);
            const auto rotated90 = Editor::updateGizmoDrag(planarDrag, scene.camera(), extent, region, ringPlusY);
            EXPECT_TRUE(isFiniteTransform(rotated90));
            const glm::vec3 xAfter = rotated90.rotation() * glm::vec3(1.0f, 0.0f, 0.0f);
            EXPECT_NEAR(xAfter.x, 0.0f, 1e-2f);
            EXPECT_NEAR(xAfter.y, 1.0f, 1e-2f);
            EXPECT_NEAR(xAfter.z, 0.0f, 1e-2f);

            // 掠射视角锁定 Y 轴切线分支，即使途中光标大幅上下移动跨越 0.15 阈值也不切换分支或跳变
            const glm::vec2 ringCenter = projectWorldPoint({0.0, 0.0, 0.0});
            auto grazingDrag = Editor::beginGizmoDrag(id, Editor::GizmoMode::Rotate, Editor::GizmoAxis::Y,
                                                      entity->transform, scene.camera(), extent, region, ringCenter);
            ASSERT_TRUE(grazingDrag.active);
            EXPECT_FALSE(grazingDrag.planarMode);
            const glm::vec2 tangentPx(grazingDrag.startScreenTangent);
            const auto step1 = Editor::updateGizmoDrag(grazingDrag, scene.camera(), extent, region,
                                                       ringCenter + tangentPx * 45.0f + glm::vec2(0.0f, 200.0f));
            const auto step2 = Editor::updateGizmoDrag(grazingDrag, scene.camera(), extent, region,
                                                       ringCenter + tangentPx * 46.0f + glm::vec2(0.0f, 200.0f));
            EXPECT_TRUE(isFiniteTransform(step1));
            EXPECT_TRUE(isFiniteTransform(step2));
            EXPECT_FALSE(grazingDrag.planarMode);
            const glm::vec3 z1 = step1.rotation() * glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 z2 = step2.rotation() * glm::vec3(0.0f, 0.0f, 1.0f);
            EXPECT_GT(z1.x, 0.2f);
            EXPECT_NEAR(glm::length(z2 - z1), 1.0f / 90.0f, 0.02f);
        }

        // 3. Scale 模式：旋转局部轴缩放与零初始分量加性恢复
        {
            Scene::Scene scene;
            scene.setCamera(Scene::Camera({2.0f, 2.0f, 6.0f}));
            const glm::quat rotY90 = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
            const auto id = scene.createEntity(nullptr, Scene::Transform({0.0f, 0.0f, 0.0f}, rotY90, {0.0f, 2.0f, 1.0f}),
                                               true, "ScaleTarget");
            const auto* entity = scene.find(id);
            ASSERT_NE(entity, nullptr);

            const auto gizmo = Editor::projectGizmo(scene, id, Editor::GizmoMode::Scale, extent, region);
            ASSERT_TRUE(gizmo.visible);

            const glm::vec2 xStart = gizmo.segments[0].segment.start;
            const glm::vec2 xEnd = gizmo.segments[0].segment.end;
            auto zeroScaleDrag = Editor::beginGizmoDrag(id, Editor::GizmoMode::Scale, Editor::GizmoAxis::X,
                                                        entity->transform, scene.camera(), extent, region, xStart);
            ASSERT_TRUE(zeroScaleDrag.active);
            const auto zeroRecovered =
                Editor::updateGizmoDrag(zeroScaleDrag, scene.camera(), extent, region, xEnd);
            EXPECT_TRUE(isFiniteTransform(zeroRecovered));
            EXPECT_GT(zeroRecovered.scale().x, 0.5f);
            EXPECT_FLOAT_EQ(zeroRecovered.scale().y, 2.0f);
            EXPECT_FLOAT_EQ(zeroRecovered.scale().z, 1.0f);
            EXPECT_EQ(zeroRecovered.position(), entity->transform.position());
            EXPECT_EQ(zeroRecovered.rotation(), entity->transform.rotation());
        }
    }

    TEST_F(EditorAppTest, GizmoDragGesturePriorityUndoRedoOutOfViewportAndCancel)
    {
        copyShowcaseModels();
        auto opts = options();
        opts.window.width = 1280;
        opts.window.height = 800;
        Editor::EditorApp session(opts);
        Editor::EditorScene editor;

        Scene::EntityId targetId;
        Scene::EntityId otherId;
        Scene::Transform initialTransform;
        Scene::Transform postDragTransform;
        Scene::Camera initialCamera;
        glm::vec2 xTipScreen{};
        glm::vec2 xMidScreen{};
        GLFWwindowfocusfun focus = nullptr;
        int step = 0;
        int totalFrames = 0;
        bool done = false;

        auto makeViewportInput = [&](const App::ViewportRegion& region, glm::vec2 screenPos, bool captureMouse,
                                     bool pressed, bool down, bool released, bool focus = false) {
            App::UiInput ui;
            ui.position = {screenPos.x - region.origin.x, screenPos.y - region.origin.y};
            ui.captureMouse = captureMouse;
            ui.select = {down, pressed, released};
            ui.focusPressed = focus;
            return ui;
        };

        session.run(
            {[&](auto& app) {
                 editor.initialize(app, "assets/scenes/mixed.json");
                 ASSERT_GE(app.scene().entities().size(), 2u);
                 targetId = app.scene().entities()[0].id;
                 otherId = app.scene().entities()[1].id;
                 editor.select(targetId);
                 initialTransform = app.scene().find(targetId)->transform;
                 initialCamera = app.scene().camera();
                 EXPECT_FALSE(editor.isDirty());
                 auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                 focus = glfwSetWindowFocusCallback(handle, nullptr);
                 glfwSetWindowFocusCallback(handle, focus);
                 focus(handle, GLFW_TRUE);
             },
             [&](auto& app, const auto&) {
                 if (++totalFrames > 160)
                 {
                     ADD_FAILURE() << "GizmoDragGesturePriorityUndoRedoOutOfViewportAndCancel 超时 step=" << step;
                     app.requestExit();
                 }
             },
             [&](auto& app) {
                 editor.ui(app);
                 if (step == 6)
                 {
                     // 第 N 帧 input 末尾刚投递失焦事件，等待第 N+1 帧 cancelInteraction 执行后再断言
                     ++step;
                 }
                 else if (step == 7)
                 {
                     // 验证失焦引发的 AppSession::cancelInteraction 已将变换精确回滚且无残留 Undo 记录
                     EXPECT_FALSE(editor.gizmoDragging());
                     EXPECT_EQ(app.scene().find(targetId)->transform.position(), initialTransform.position());
                     EXPECT_FALSE(editor.canUndo());
                     EXPECT_FALSE(editor.isDirty());
                     auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                     focus(handle, GLFW_TRUE);
                     ++step;
                 }
             },
             [&](auto& app) { editor.cancelInteraction(app); },
             [&](auto& app, auto extent, const auto& realInput) {
                 const auto& region = app.viewportRegion();
                 if (extent.width == 0 || extent.height == 0 || region.size.x <= 0.0 || region.size.y <= 0.0)
                 {
                     editor.input(app, extent, realInput);
                     return;
                 }
                 switch (step)
                 {
                     case 0:
                     {
                         const auto gizmo =
                             Editor::projectGizmo(app.scene(), targetId, editor.gizmoMode(), extent, region);
                         ASSERT_TRUE(gizmo.visible);
                         xMidScreen = (gizmo.segments[0].segment.start + gizmo.segments[0].segment.end) * 0.5f;
                         xTipScreen = gizmo.segments[0].segment.end;
                         // 从视口外按下并拖入视口不应误启动 Gizmo 拖拽
                         editor.input(app, extent,
                                      makeViewportInput(region, glm::vec2(region.origin.x - 20.0f, xMidScreen.y), true,
                                                        true, true, false));
                         EXPECT_FALSE(editor.gizmoDragging());
                         ++step;
                         break;
                     }
                     case 1:
                         editor.input(app, extent, makeViewportInput(region, xMidScreen, false, false, true, false));
                         EXPECT_FALSE(editor.gizmoDragging());
                         editor.input(app, extent, makeViewportInput(region, xMidScreen, false, false, false, true));
                         EXPECT_EQ(editor.selected(), targetId);
                         ++step;
                         break;
                     case 2:
                         // 在视口内左键按下命中 X 轴启动 Gizmo 拖拽，且阻断相机/点选
                         editor.input(app, extent, makeViewportInput(region, xMidScreen, false, true, true, false));
                         EXPECT_TRUE(editor.gizmoDragging());
                         EXPECT_EQ(editor.activeGizmoAxis(), Editor::GizmoAxis::X);
                         // 拖拽中调用 setGizmoMode 不应切换正在进行的平移拖拽
                         editor.setGizmoMode(Editor::GizmoMode::Rotate);
                         EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Translate);
                         ++step;
                         break;
                     case 3:
                         // 拖到视口外（captureMouse=true）同时按 F 键：拖拽持续有效且相机不动
                         editor.input(app, extent,
                                      makeViewportInput(region,
                                                        glm::vec2(region.origin.x + region.size.x + 120.0f, xTipScreen.y),
                                                        true, false, true, false, true));
                         EXPECT_TRUE(editor.gizmoDragging());
                         EXPECT_GT(app.scene().find(targetId)->transform.position().x,
                                   initialTransform.position().x + 0.5f);
                         EXPECT_EQ(app.scene().camera().position(), initialCamera.position());
                         ++step;
                         break;
                     case 4:
                         // 松开左键即使在视口外也正常提交为单条 Undo 记录，且不误取消选择
                         editor.input(app, extent,
                                      makeViewportInput(region,
                                                        glm::vec2(region.origin.x + region.size.x + 120.0f, xTipScreen.y),
                                                        true, false, false, true));
                         EXPECT_FALSE(editor.gizmoDragging());
                         EXPECT_EQ(editor.selected(), targetId);
                         postDragTransform = app.scene().find(targetId)->transform;
                         EXPECT_TRUE(editor.isDirty());
                         EXPECT_TRUE(editor.canUndo());
                         ASSERT_TRUE(editor.undo(app));
                         EXPECT_FALSE(editor.canUndo());
                         EXPECT_FALSE(editor.isDirty());
                         EXPECT_EQ(app.scene().find(targetId)->transform.position(), initialTransform.position());
                         ASSERT_TRUE(editor.redo(app));
                         EXPECT_TRUE(editor.isDirty());
                         EXPECT_EQ(app.scene().find(targetId)->transform.position(), postDragTransform.position());
                         ASSERT_TRUE(editor.undo(app));
                         EXPECT_FALSE(editor.canUndo());
                         EXPECT_FALSE(editor.isDirty());
                         ++step;
                         break;
                     case 5:
                     {
                         // 重新开始 Gizmo 拖拽后模拟窗口失焦：由 AppSession 触发 cancelInteraction 回滚
                         const auto gizmo =
                             Editor::projectGizmo(app.scene(), targetId, editor.gizmoMode(), extent, region);
                         xMidScreen = (gizmo.segments[0].segment.start + gizmo.segments[0].segment.end) * 0.5f;
                         editor.input(app, extent, makeViewportInput(region, xMidScreen, false, true, true, false));
                         ASSERT_TRUE(editor.gizmoDragging());
                         editor.input(app, extent,
                                      makeViewportInput(region, xMidScreen + glm::vec2(60.0f, 0.0f), false, false, true,
                                                        false));
                         EXPECT_GT(app.scene().find(targetId)->transform.position().x,
                                   initialTransform.position().x + 0.1f);
                         auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         focus(handle, GLFW_FALSE);
                         ++step;
                         break;
                     }
                     case 6:
                     case 7:
                         // 等待第 N+1 帧 cancelInteraction 执行并由 ui 恢复窗口焦点
                         break;
                     case 8:
                     {
                         // 拖拽中切换选中实体也应立即回滚取消事务
                         const auto gizmo =
                             Editor::projectGizmo(app.scene(), targetId, editor.gizmoMode(), extent, region);
                         xMidScreen = (gizmo.segments[0].segment.start + gizmo.segments[0].segment.end) * 0.5f;
                         editor.input(app, extent, makeViewportInput(region, xMidScreen, false, true, true, false));
                         ASSERT_TRUE(editor.gizmoDragging());
                         editor.input(app, extent,
                                      makeViewportInput(region, xMidScreen + glm::vec2(60.0f, 0.0f), false, false, true,
                                                        false));
                         editor.select(app, otherId);
                         EXPECT_FALSE(editor.gizmoDragging());
                         EXPECT_EQ(editor.selected(), otherId);
                         EXPECT_EQ(app.scene().find(targetId)->transform.position(), initialTransform.position());
                         EXPECT_FALSE(editor.canUndo());
                         ++step;
                         break;
                     }
                     case 9:
                     {
                         // 未命中 Gizmo 轴的空白处左键点选仍保持原有取消选择行为
                         const glm::vec2 blankPos(region.origin.x + 4.0, region.origin.y + 4.0);
                         editor.input(app, extent, makeViewportInput(region, blankPos, false, true, true, false));
                         EXPECT_FALSE(editor.gizmoDragging());
                         editor.input(app, extent, makeViewportInput(region, blankPos, false, false, false, true));
                         EXPECT_FALSE(editor.selected().valid());
                         done = true;
                         app.requestExit();
                         break;
                     }
                 }
             },
             [&](auto& app) { editor.layout(app); }, [&](auto& app) { editor.requestClose(app); }});

        EXPECT_TRUE(done);
        cleanValidation();
    }

    TEST_F(EditorAppTest, GizmoOverlayClipOrderColorsHighlightToolbarAndShortcuts)
    {
        copyShowcaseModels();
        auto config = options();
        config.window.width = 1280;
        config.window.height = 800;
        Editor::EditorApp session(config);
        Editor::EditorScene editor;
        Scene::EntityId modeledId{};
        Scene::EntityId emptyEntityId{};
        int frame = 0;
        int step = 0;
        bool done = false;
        glm::vec2 xMidScreen{0.0f};
        bool triggerEmptyDragThisFrame = false;
        ImGuiID pendingNavActivateId = 0;
        ImGuiID pendingItemActivateId = 0;
        float emptyInitialAvgX = 0.0f;
        int rotateVertexCount = 0;
        int translateVertexCount = 0;

        constexpr ImU32 kBoxColor = IM_COL32(255, 200, 64, 255);
        constexpr ImU32 kXColor = IM_COL32(230, 70, 70, 255);
        constexpr ImU32 kYColor = IM_COL32(70, 210, 90, 255);
        constexpr ImU32 kZColor = IM_COL32(70, 140, 245, 255);
        constexpr ImU32 kHighlightColor = IM_COL32(255, 240, 100, 255);

        struct DrawColorSummary
        {
            int boxVertices = 0;
            int xVertices = 0;
            int yVertices = 0;
            int zVertices = 0;
            int highlightVertices = 0;
            int maxBoxIndex = -1;
            int minGizmoIndex = -1;
            int maxDefaultAxisIndex = -1;
            int minHighlightIndex = -1;
            float avgXPosForHighlight = 0.0f;
            float avgXPosForGizmo = 0.0f;
            bool allOverlayClippedToViewport = false;
        };

        const auto summarizeDrawList = [&](const ImDrawList* drawList, const App::ViewportRegion& region) {
            DrawColorSummary out{};
            if (!drawList)
                return out;
            double highlightXSum = 0.0;
            double gizmoXSum = 0.0;
            int gizmoCount = 0;
            for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
            {
                const auto& v = drawList->VtxBuffer[i];
                if (v.col == kBoxColor)
                {
                    ++out.boxVertices;
                    out.maxBoxIndex = std::max(out.maxBoxIndex, i);
                }
                else if (v.col == kXColor || v.col == kYColor || v.col == kZColor || v.col == kHighlightColor)
                {
                    if (v.col == kXColor)
                        ++out.xVertices;
                    else if (v.col == kYColor)
                        ++out.yVertices;
                    else if (v.col == kZColor)
                        ++out.zVertices;
                    else
                    {
                        ++out.highlightVertices;
                        highlightXSum += v.pos.x;
                        out.minHighlightIndex =
                            out.minHighlightIndex < 0 ? i : std::min(out.minHighlightIndex, i);
                    }
                    if (v.col != kHighlightColor)
                        out.maxDefaultAxisIndex = std::max(out.maxDefaultAxisIndex, i);
                    out.minGizmoIndex = out.minGizmoIndex < 0 ? i : std::min(out.minGizmoIndex, i);
                    gizmoXSum += v.pos.x;
                    ++gizmoCount;
                }
            }
            if (out.highlightVertices > 0)
                out.avgXPosForHighlight = static_cast<float>(highlightXSum / out.highlightVertices);
            if (gizmoCount > 0)
                out.avgXPosForGizmo = static_cast<float>(gizmoXSum / gizmoCount);

            const float rx0 = static_cast<float>(region.origin.x);
            const float ry0 = static_cast<float>(region.origin.y);
            const float rx1 = static_cast<float>(region.origin.x + region.size.x);
            const float ry1 = static_cast<float>(region.origin.y + region.size.y);
            for (int c = 0; c < drawList->CmdBuffer.Size; ++c)
            {
                const auto& cmd = drawList->CmdBuffer[c];
                if (std::abs(cmd.ClipRect.x - rx0) < 1.0f && std::abs(cmd.ClipRect.y - ry0) < 1.0f &&
                    std::abs(cmd.ClipRect.z - rx1) < 1.0f && std::abs(cmd.ClipRect.w - ry1) < 1.0f)
                {
                    out.allOverlayClippedToViewport = true;
                    break;
                }
            }
            return out;
        };

        const auto makeViewportInput = [](const App::ViewportRegion& region, glm::vec2 screenPos, bool hovered,
                                          bool selectPressed, bool selectDown, bool selectReleased) {
            App::UiInput uiInput{};
            uiInput.position = {screenPos.x - region.origin.x, screenPos.y - region.origin.y};
            uiInput.select = {selectDown, selectPressed, selectReleased};
            uiInput.captureMouse = !hovered;
            uiInput.captureKeyboard = false;
            return uiInput;
        };

        auto* handle = static_cast<GLFWwindow*>(session.window().getNativeHandle());
        GLFWwindowfocusfun focus = glfwSetWindowFocusCallback(handle, nullptr);
        glfwSetWindowFocusCallback(handle, focus);

        session.run(
            {[&](auto& app) {
                 editor.initialize(app);
                 EditorTest::setSharedEditorScene(app, editor, false);
                 modeledId = editor.selected();
                 Scene::Transform emptyTransform;
                 emptyTransform.setPosition({0.5f, 0.25f, 0.0f});
                 emptyEntityId = app.scene().createEntity({}, emptyTransform, true, "EmptyMarker");
                 editor.select(app, modeledId);
                 focus(handle, GLFW_TRUE);
             },
             [&](auto& app, const auto&) {
                 if (++frame > 100)
                 {
                     ADD_FAILURE() << "GizmoOverlayClipOrderColorsHighlightToolbarAndShortcuts 超时 step=" << step;
                     app.requestExit();
                 }
                 if (pendingItemActivateId != 0)
                 {
                     ImGui::ActivateItemByID(pendingItemActivateId);
                     pendingItemActivateId = 0;
                 }
                 if (triggerEmptyDragThisFrame)
                 {
                     triggerEmptyDragThisFrame = false;
                     const auto extent = app.sceneExtent();
                     const auto& region = app.viewportRegion();
                     const auto gizmo =
                         Editor::projectGizmo(app.scene(), emptyEntityId, Editor::GizmoMode::Translate, extent, region);
                     ASSERT_FALSE(gizmo.segments.empty());
                     const glm::vec2 emptyXMid =
                         (gizmo.segments[0].segment.start + gizmo.segments[0].segment.end) * 0.5f;
                     editor.input(app, extent, makeViewportInput(region, emptyXMid, true, true, true, false));
                     ASSERT_TRUE(editor.gizmoDragging());
                     editor.input(
                         app, extent,
                         makeViewportInput(region, emptyXMid + glm::vec2(80.0f, 0.0f), true, false, true, false));
                 }
             },
             [&](auto& app) {
                 if (pendingNavActivateId != 0)
                 {
                     auto& context = *ImGui::GetCurrentContext();
                     context.NavActivateId = pendingNavActivateId;
                     context.NavActivateDownId = pendingNavActivateId;
                     pendingNavActivateId = 0;
                 }
                 editor.ui(app);
                 if (done)
                     return;
                 const auto extent = app.sceneExtent();
                 const auto& region = app.viewportRegion();
                 if (frame < 3 || extent.width == 0 || extent.height == 0 || region.size.x <= 0 || region.size.y <= 0)
                     return;

                 const auto summary = summarizeDrawList(app.sceneDrawList(), region);
                 switch (step)
                 {
                     case 0:
                     {
                         // 1. 有模型实体默认 Translate 模式：AABB 先画，三轴 Gizmo 叠在其上，视口裁剪正确
                         EXPECT_GT(summary.boxVertices, 0);
                         EXPECT_GT(summary.xVertices, 0);
                         EXPECT_GT(summary.yVertices, 0);
                         EXPECT_GT(summary.zVertices, 0);
                         EXPECT_EQ(summary.highlightVertices, 0);
                         EXPECT_LT(summary.maxBoxIndex, summary.minGizmoIndex);
                         EXPECT_TRUE(summary.allOverlayClippedToViewport);
                         translateVertexCount = summary.xVertices + summary.yVertices + summary.zVertices;

                         // 悬停 X 轴：下帧绘制中 X 轴变高亮，其余 Y/Z 保持蓝/绿
                         const auto gizmo =
                             Editor::projectGizmo(app.scene(), modeledId, Editor::GizmoMode::Translate, extent, region);
                         ASSERT_FALSE(gizmo.segments.empty());
                         xMidScreen = (gizmo.segments[0].segment.start + gizmo.segments[0].segment.end) * 0.5f;
                         editor.input(app, extent, makeViewportInput(region, xMidScreen, true, false, false, false));
                         EXPECT_EQ(editor.hoveredGizmoAxis(), Editor::GizmoAxis::X);
                         ++step;
                         break;
                     }
                     case 1:
                     {
                         // 验证悬停 X 轴时 X 轴切换为高亮黄白且绘制在 Y/Z 之上
                         EXPECT_EQ(summary.xVertices, 0);
                         EXPECT_GT(summary.highlightVertices, 0);
                         EXPECT_GT(summary.yVertices, 0);
                         EXPECT_GT(summary.zVertices, 0);
                         EXPECT_GT(summary.minHighlightIndex, summary.maxDefaultAxisIndex);

                         // 开始按住左键拖拽 Gizmo，同时按 E 不应切换模式
                         editor.input(app, extent, makeViewportInput(region, xMidScreen, true, true, true, false));
                         ASSERT_TRUE(editor.gizmoDragging());
                         EXPECT_EQ(editor.activeGizmoAxis(), Editor::GizmoAxis::X);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_E, true);
                         ++step;
                         break;
                     }
                     case 2:
                     {
                         // 验证拖拽期间 E 键被忽略，随后释放 E 与左键，并在同一帧触发 E 切换到 Rotate
                         EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Translate);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_E, false);
                         const glm::vec2 shifted = xMidScreen + glm::vec2(70.0f, 0.0f);
                         editor.input(app, extent, makeViewportInput(region, shifted, true, false, true, false));
                         EXPECT_EQ(editor.activeGizmoAxis(), Editor::GizmoAxis::X);
                         editor.input(app, extent, makeViewportInput(region, shifted, true, false, false, true));
                         EXPECT_FALSE(editor.gizmoDragging());

                         // 移开光标后恢复默认红/绿/蓝配色
                         const glm::vec2 blankPos(region.origin.x + 5.0, region.origin.y + 5.0);
                         editor.input(app, extent, makeViewportInput(region, blankPos, true, false, false, false));
                         EXPECT_EQ(editor.hoveredGizmoAxis(), Editor::GizmoAxis::None);
                         ++step;
                         break;
                     }
                     case 3:
                     {
                         EXPECT_GT(summary.xVertices, 0);
                         EXPECT_GT(summary.yVertices, 0);
                         EXPECT_GT(summary.zVertices, 0);
                         EXPECT_EQ(summary.highlightVertices, 0);

                         // 2. 通过 Properties 面板按钮切换到 Rotate (E)
                         auto* props = ImGui::FindWindowByName("Properties");
                         ASSERT_NE(props, nullptr);
                         pendingNavActivateId = props->GetID("Rotate (E)");
                         ++step;
                         break;
                     }
                     case 4:
                     {
                         EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Rotate);
                         rotateVertexCount = summary.xVertices + summary.yVertices + summary.zVertices;
                         EXPECT_GT(rotateVertexCount, translateVertexCount);

                         // 通过面板按钮切换到 Scale (R)，再通过 W 键切回 Translate，R 键切到 Scale
                         auto* props = ImGui::FindWindowByName("Properties");
                         ASSERT_NE(props, nullptr);
                         pendingNavActivateId = props->GetID("Scale (R)");
                         ++step;
                         break;
                     }
                     case 5:
                     {
                         EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Scale);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
                         ++step;
                         break;
                     }
                     case 6:
                     {
                         EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Translate);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_R, true);
                         ++step;
                         break;
                     }
                     case 7:
                     {
                         EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Scale);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_R, false);

                         // 带 Ctrl / Alt 修饰键按 W 不得切换模式
                         ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
                         ++step;
                         break;
                     }
                     case 8:
                     {
                         EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Scale);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
                         ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);

                         // 激活 Name 文本框输入 'w' / 'e' 不得切换模式
                         auto* props = ImGui::FindWindowByName("Properties");
                         ASSERT_NE(props, nullptr);
                         const std::string idStr = std::to_string(modeledId.value());
                         pendingItemActivateId = ImHashStr("##entity-name", 0, props->GetID(idStr.c_str()));
                         ++step;
                         break;
                     }
                     case 9:
                     {
                         // 本帧 editor.ui 已激活 ##entity-name，下帧 WantTextInput == true 时注入 W
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
                         ImGui::GetIO().AddInputCharacter('w');
                         ++step;
                         break;
                     }
                     case 10:
                     {
                         EXPECT_TRUE(ImGui::GetIO().WantTextInput);
                         EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Scale);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
                         ++step;
                         break;
                     }
                     case 11:
                     {
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
                         ImGui::ClearActiveID();

                         // 光标置于视口图像区域内并按住右键（相机轨道拖拽态）
                         ImGui::GetIO().AddMousePosEvent(static_cast<float>(region.origin.x + 24.0),
                                                         static_cast<float>(region.origin.y + 24.0));
                         ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
                         ++step;
                         break;
                     }
                     case 12:
                     {
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
                         ++step;
                         break;
                     }
                     case 13:
                     {
                         EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Scale);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
                         ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
                         ++step;
                         break;
                     }
                     case 14:
                     {
                         // 等待鼠标右键与 W 键释放事件排空后，再按 W 恢复 Translate
                         if (!ImGui::GetCurrentContext()->InputEventsQueue.empty() ||
                             ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsKeyDown(ImGuiKey_W))
                             break;
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
                         ++step;
                         break;
                     }
                     case 15:
                     {
                         EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Translate);
                         ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);

                         // 3. 选中无模型实体：无 AABB，但三轴 Gizmo 正常显示且可拖拽
                         editor.select(app, emptyEntityId);
                         ++step;
                         break;
                     }
                     case 16:
                     {
                         EXPECT_EQ(summary.boxVertices, 0);
                         EXPECT_GT(summary.xVertices, 0);
                         EXPECT_GT(summary.yVertices, 0);
                         EXPECT_GT(summary.zVertices, 0);
                         emptyInitialAvgX = summary.avgXPosForGizmo;

                         // 下帧 update 中对无模型实体拖拽 X 轴，同帧 ui 立即看到位置与高亮更新（无一帧滞后）
                         triggerEmptyDragThisFrame = true;
                         ++step;
                         break;
                     }
                     case 17:
                     {
                         const auto* emptyEntity = app.scene().find(emptyEntityId);
                         ASSERT_NE(emptyEntity, nullptr);
                         EXPECT_GT(emptyEntity->transform.position().x, 0.6f);
                         EXPECT_GT(summary.avgXPosForGizmo, emptyInitialAvgX + 10.0f);
                         EXPECT_EQ(summary.xVertices, 0);
                         EXPECT_GT(summary.highlightVertices, 0);

                         // 结束拖拽并隐藏无模型实体 -> 下帧 Gizmo 立即消失
                         editor.input(app, extent, makeViewportInput(region, xMidScreen, true, false, false, true));
                         EXPECT_FALSE(editor.gizmoDragging());
                         ASSERT_TRUE(app.scene().setVisible(emptyEntityId, false));
                         ++step;
                         break;
                     }
                     case 18:
                     {
                         EXPECT_EQ(summary.boxVertices, 0);
                         EXPECT_EQ(summary.xVertices, 0);
                         EXPECT_EQ(summary.yVertices, 0);
                         EXPECT_EQ(summary.zVertices, 0);

                         // 重新显示后恢复，再测试清空/取消选中立即消失
                         ASSERT_TRUE(app.scene().setVisible(emptyEntityId, true));
                         ++step;
                         break;
                     }
                     case 19:
                     {
                         EXPECT_GT(summary.xVertices, 0);
                         editor.select(app, {});
                         ++step;
                         break;
                     }
                     case 20:
                     {
                         EXPECT_EQ(summary.boxVertices, 0);
                         EXPECT_EQ(summary.xVertices, 0);
                         EXPECT_EQ(summary.yVertices, 0);
                         EXPECT_EQ(summary.zVertices, 0);

                         // 验证 UiInputAdapter 失焦取消后若 W 仍被按住，不会提前结束 neutral suspended
                         App::UiInputAdapter adapter;
                         Input::InputState raw;
                         raw.onFocus(true);
                         raw.onKey(Input::KeyCode::W, Input::KeyState::Press);
                         ImGui::GetIO().MousePos = {42.0f, 24.0f};
                         adapter.cancel();
                         const auto blocked = adapter.sample(raw);
                         EXPECT_EQ(blocked.position.x, 0.0);
                         raw.onKey(Input::KeyCode::W, Input::KeyState::Release);
                         const auto neutralFrame = adapter.sample(raw);
                         EXPECT_EQ(neutralFrame.position.x, 0.0);
                         const auto resumed = adapter.sample(raw);
                         EXPECT_EQ(resumed.position.x, 42.0);

                         done = true;
                         app.requestExit();
                         break;
                     }
                 }
             },
             {}, {}, [&](auto& app) { editor.layout(app); }, [&](auto& app) { editor.requestClose(app); }});

        EXPECT_TRUE(done);
        cleanValidation();
    }

    TEST_F(EditorAppTest, GizmoThreeModeVisualScreenshotsWithRequiredValidation)
    {
        copyShowcaseModels();
        auto config = options(true);
        config.window.width = 1280;
        config.window.height = 800;
        config.validation = Render::RendererValidationMode::Required;
        Editor::EditorApp session(config);
        Editor::EditorScene editor;
        int frame = 0;
        WindowCapture::Image translateImg, rotateImg, scaleImg;
        bool capturedAll = false;

        session.run(
            {[&](auto& app) {
                 editor.initialize(app, "assets/scenes/shadows.json");
                 app.shadowSettings().enabled = true;
                 ASSERT_FALSE(app.scene().entities().empty());
                 editor.select(app, app.scene().entities().front().id);
                 editor.setGizmoMode(Editor::GizmoMode::Translate);
                 std::filesystem::create_directories("review");
             },
             [&](auto& app, const auto&) {
                 ++frame;
                 if (frame == 5)
                 {
                     glfwWaitEventsTimeout(0.06);
                     translateImg = WindowCapture::capture(app.window());
                     WindowCapture::save(translateImg, "review/gizmo-translate.ppm");
                     editor.setGizmoMode(Editor::GizmoMode::Rotate);
                 }
                 else if (frame == 7)
                 {
                     glfwWaitEventsTimeout(0.06);
                     rotateImg = WindowCapture::capture(app.window());
                     WindowCapture::save(rotateImg, "review/gizmo-rotate.ppm");
                     editor.setGizmoMode(Editor::GizmoMode::Scale);
                 }
                 else if (frame == 9)
                 {
                     glfwWaitEventsTimeout(0.06);
                     scaleImg = WindowCapture::capture(app.window());
                     WindowCapture::save(scaleImg, "review/gizmo-scale.ppm");
                     capturedAll = true;
                     app.requestExit();
                 }
             },
             [&](auto& app) { editor.ui(app); },
             {},
             {},
             [&](auto& app) { editor.layout(app); },
             [&](auto& app) { editor.requestClose(app); }});

        EXPECT_TRUE(capturedAll);
        EXPECT_GT(translateImg.width, 0);
        EXPECT_GT(translateImg.height, 0);
        EXPECT_EQ(translateImg.width, rotateImg.width);
        EXPECT_EQ(translateImg.height, rotateImg.height);
        EXPECT_EQ(translateImg.width, scaleImg.width);
        EXPECT_EQ(translateImg.height, scaleImg.height);
        EXPECT_FALSE(translateImg.rgba.empty());
        EXPECT_NE(translateImg.rgba, rotateImg.rgba);
        EXPECT_NE(rotateImg.rgba, scaleImg.rgba);
        EXPECT_NE(translateImg.rgba, scaleImg.rgba);
        EXPECT_TRUE(std::filesystem::exists("review/gizmo-translate.ppm"));
        EXPECT_TRUE(std::filesystem::exists("review/gizmo-rotate.ppm"));
        EXPECT_TRUE(std::filesystem::exists("review/gizmo-scale.ppm"));
        cleanValidation();
    }

}  // namespace
