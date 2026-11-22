// 编译期确认控制器、操作接口与 Gizmo 头文件独立于 ImGui 与 Vulkan。
#include "app/editor/editor_controller.h"
#include "app/editor/editor_operations.h"
#include "app/editor/gizmo.h"

#ifdef IMGUI_VERSION
#error "editor_controller.h and viewport_input.h must not include imgui.h"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
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
        EXPECT_FALSE(operations.clear(scene, clearColor));
        EXPECT_FALSE(operations.remove(scene, clearColor));
        EXPECT_FALSE(operations.duplicate(scene, clearColor).valid());
        EXPECT_FALSE(operations.isDirty());

        const auto first = scene.createEntity({}, Scene::Transform({1, 0, 0}), true, "对象A");
        const auto second = scene.createEntity({}, Scene::Transform({2, 0, 0}), true, "对象B");

        // 仅切换选择或写入相同值均不置脏。
        operations.select(first);
        EXPECT_EQ(operations.selected(), first);
        EXPECT_TRUE(operations.rename(scene, clearColor, "对象A"));
        EXPECT_TRUE(operations.setVisible(scene, clearColor, true));
        EXPECT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({1, 0, 0})));
        EXPECT_TRUE(operations.setLighting(scene, clearColor, scene.lighting()));
        EXPECT_TRUE(operations.setDisplay(scene, clearColor, scene.display()));
        EXPECT_TRUE(operations.setClearColor(scene, clearColor, clearColor));
        EXPECT_FALSE(operations.isDirty());

        // 真实修改（名称、可见性、TRS、灯光、背景色、复制、删除、清空）均正确置脏。
        const auto checkDirty = [&](auto fn) {
            Editor::EditorOperations op; Scene::Scene s;
            std::array<float, 4> bg{0.1f, 0.2f, 0.3f, 1.0f};
            op.select(s.createEntity({}, Scene::Transform({1, 0, 0}), true, "初始"));
            EXPECT_FALSE(op.isDirty());
            fn(op, s, bg);
            EXPECT_TRUE(op.isDirty());
        };
        checkDirty([](auto& op, auto& s, const auto& bg) { EXPECT_TRUE(op.rename(s, bg, "新名称")); });
        checkDirty([](auto& op, auto& s, const auto& bg) { EXPECT_TRUE(op.setVisible(s, bg, false)); });
        checkDirty([](auto& op, auto& s, const auto& bg) { EXPECT_TRUE(op.setTransform(s, bg, Scene::Transform({3, 4, 5}))); });
        checkDirty([](auto& op, auto& s, const auto& bg) { EXPECT_TRUE(op.duplicate(s, bg).valid()); });
        checkDirty([](auto& op, auto& s, const auto& bg) { EXPECT_TRUE(op.remove(s, bg)); });
        checkDirty([](auto& op, auto& s, const auto& bg) { EXPECT_TRUE(op.clear(s, bg)); });
        checkDirty([](auto& op, auto& s, const auto& bg) { auto l = s.lighting(); l.intensity = 2.5f; EXPECT_TRUE(op.setLighting(s, bg, l)); });
        checkDirty([](auto& op, auto& s, const auto& bg) {
            EXPECT_TRUE(op.setDisplay(s, bg, {Render::ToneMappingMode::Off, 2.0f}));
        });
        checkDirty([](auto& op, const auto& s, auto& bg) { EXPECT_TRUE(op.setClearColor(s, bg, {0.5f, 0.5f, 0.5f, 1.0f})); });

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
        EXPECT_FALSE(operations.canUndo() || operations.canRedo() || operations.isDirty());

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
        // 异常输入不污染历史栈。
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

        operations.select(idCopy);
        EXPECT_FALSE(operations.rename(scene, clearColor, "missing"));

        // 逐步撤销全部 8 次离散编辑并检查恢复状态及初始修订脏状态。
        ASSERT_TRUE(operations.undo(scene, clearColor));
        ASSERT_EQ(scene.entities().size(), 2u);
        EXPECT_TRUE(scene.entities()[0].id == idB && scene.entities()[1].id == idCopy);

        ASSERT_TRUE(operations.undo(scene, clearColor));
        ASSERT_EQ(scene.entities().size(), 3u);
        EXPECT_TRUE(scene.entities()[0].id == idA && scene.entities()[0].name == "重命名A" &&
                    !scene.entities()[0].visible && scene.entities()[0].transform.position() == glm::vec3(9, 8, 7) &&
                    operations.selected() == idA);

        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_EQ(clearColor, (std::array<float, 4>{0.12f, 0.16f, 0.22f, 1.0f}));
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_FLOAT_EQ(scene.lighting().intensity, 1.0f);
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_TRUE(scene.find(idCopy) == nullptr && operations.selected() == idA);

        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_EQ(scene.find(idA)->transform.position(), glm::vec3(1, 0, 0));
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_TRUE(scene.find(idA)->visible && operations.isDirty());
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_TRUE(scene.find(idA)->name == "原始A" && !operations.isDirty() && !operations.canUndo() && operations.canRedo());

        // 之后新建实体 ID 不与历史 Copy 冲突。
        const auto idNew = scene.createEntity();
        EXPECT_GT(idNew.value(), idCopy.value());
        EXPECT_TRUE(scene.destroyEntity(idNew));

        // 连续重做前 2 步后执行新编辑应截断 redo 栈。
        ASSERT_TRUE(operations.redo(scene, clearColor));
        EXPECT_TRUE(operations.isDirty() && scene.find(idA)->name == "重命名A");
        ASSERT_TRUE(operations.redo(scene, clearColor));
        EXPECT_TRUE(!scene.find(idA)->visible && operations.canRedo());
        ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({5, 5, 5})));
        EXPECT_FALSE(operations.canRedo());

        // 容量上限：65 次编辑仅保留最近 MAX_HISTORY 步，且最旧基线淘汰后保持 dirty。
        Editor::EditorOperations bounded; Scene::Scene boundedScene;
        std::array<float, 4> boundedColor{0, 0, 0, 1};
        const auto target = boundedScene.createEntity({}, Scene::Transform({0, 0, 0}));
        bounded.select(target);
        for (int s = 1; s <= 65; ++s) ASSERT_TRUE(bounded.setTransform(boundedScene, boundedColor, Scene::Transform({float(s), 0, 0})));
        size_t undos = 0; while (bounded.undo(boundedScene, boundedColor)) ++undos;
        EXPECT_TRUE(undos == Editor::EditorOperations::MAX_HISTORY &&
                    boundedScene.find(target)->transform.position().x == 1.0f && bounded.isDirty());

        // 显示设置：离散切换记 1 步，连续事务合并为 1 步，Undo/Redo 正确恢复，Off 保留 EV。
        Editor::EditorOperations displayOps;
        Scene::Scene displayScene;
        std::array<float, 4> displayBg{0.12f, 0.16f, 0.22f, 1.0f};
        EXPECT_TRUE(displayOps.setDisplay(displayScene, displayBg, {Render::ToneMappingMode::Reinhard, 2.5f}));
        EXPECT_TRUE(displayOps.setDisplay(displayScene, displayBg, {Render::ToneMappingMode::Off, 2.5f}));
        EXPECT_TRUE(displayOps.setDisplay(displayScene, displayBg, {Render::ToneMappingMode::Reinhard, 2.5f}));
        displayOps.beginTransaction(displayScene, displayBg);
        EXPECT_TRUE(displayOps.setDisplay(displayScene, displayBg, {Render::ToneMappingMode::Reinhard, 3.0f}));
        EXPECT_TRUE(displayOps.setDisplay(displayScene, displayBg, {Render::ToneMappingMode::Reinhard, 4.0f}));
        EXPECT_TRUE(displayOps.commitTransaction(displayScene, displayBg));
        EXPECT_EQ(displayScene.display(), (Render::DisplaySettings{Render::ToneMappingMode::Reinhard, 4.0f}));
        EXPECT_TRUE(displayOps.undo(displayScene, displayBg));
        EXPECT_EQ(displayScene.display(), (Render::DisplaySettings{Render::ToneMappingMode::Reinhard, 2.5f}));
        EXPECT_TRUE(displayOps.undo(displayScene, displayBg));
        EXPECT_EQ(displayScene.display(), (Render::DisplaySettings{Render::ToneMappingMode::Off, 2.5f}));
        EXPECT_TRUE(displayOps.redo(displayScene, displayBg));
        EXPECT_EQ(displayScene.display(), (Render::DisplaySettings{Render::ToneMappingMode::Reinhard, 2.5f}));
    }

    TEST(EditorOperationsHistoryTest, TransactionsMergeNoOpDiscardAndCancelRollback)
    {
        Editor::EditorOperations operations; Scene::Scene scene;
        std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1.0f};
        const auto entityId = scene.createEntity({}, Scene::Transform({0, 0, 0}), true, "初始名称");
        operations.select(entityId);
        EXPECT_FALSE(operations.inTransaction() || operations.commitTransaction(scene, clearColor) ||
                     operations.cancelTransaction(scene, clearColor));

        // 事务内多次连续属性编辑折叠为 1 条撤销记录。
        operations.beginTransaction(scene, clearColor);
        EXPECT_TRUE(operations.inTransaction());
        ASSERT_TRUE(operations.rename(scene, clearColor, "阶段名称") && operations.rename(scene, clearColor, "最终名称") &&
                    operations.setVisible(scene, clearColor, false) &&
                    operations.setTransform(scene, clearColor, Scene::Transform({1, 2, 3})) &&
                    operations.setTransform(scene, clearColor, Scene::Transform({4, 5, 6})));
        auto lighting = scene.lighting(); lighting.intensity = 2.5f;
        ASSERT_TRUE(operations.setLighting(scene, clearColor, lighting));
        lighting.ambient = 0.45f;
        ASSERT_TRUE(operations.setLighting(scene, clearColor, lighting) &&
                    operations.setClearColor(scene, clearColor, {0.3f, 0.4f, 0.5f, 1.0f}) &&
                    operations.setClearColor(scene, clearColor, {0.6f, 0.7f, 0.8f, 1.0f}));
        EXPECT_FALSE(operations.canUndo());

        // 重复 beginTransaction 保持最初起点，提交后 1 次撤销还原全部属性。
        operations.beginTransaction(scene, clearColor);
        EXPECT_TRUE(operations.commitTransaction(scene, clearColor) && !operations.inTransaction() && operations.canUndo() && operations.isDirty());
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_TRUE(!operations.canUndo() && !operations.isDirty() && operations.canRedo() &&
                    scene.find(entityId)->name == "初始名称" && scene.find(entityId)->visible &&
                    scene.find(entityId)->transform.position() == glm::vec3(0, 0, 0) &&
                    scene.lighting().intensity == 1.0f && clearColor == (std::array<float, 4>{0.12f, 0.16f, 0.22f, 1.0f}));

        // 修改后又改回原值的无变化事务静默丢弃，不清空 redo 栈。
        operations.beginTransaction(scene, clearColor);
        ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({9, 9, 9})) &&
                    operations.setTransform(scene, clearColor, Scene::Transform({0, 0, 0})));
        EXPECT_TRUE(!operations.commitTransaction(scene, clearColor) && !operations.canUndo() && !operations.isDirty() && operations.canRedo());

        // cancelTransaction 回滚快照并保留 redo 栈。
        operations.beginTransaction(scene, clearColor);
        ASSERT_TRUE(operations.rename(scene, clearColor, "被取消的修改") &&
                    operations.setClearColor(scene, clearColor, {0.9f, 0.1f, 0.1f, 1.0f}));
        EXPECT_TRUE(operations.cancelTransaction(scene, clearColor) && !operations.inTransaction() &&
                    !operations.canUndo() && !operations.isDirty() && operations.canRedo());
        EXPECT_TRUE(scene.find(entityId)->name == "初始名称" && clearColor == (std::array<float, 4>{0.12f, 0.16f, 0.22f, 1.0f}));

        // 离散操作和 undo 在事务挂起时自动先结算再执行。
        ASSERT_TRUE(operations.redo(scene, clearColor));
        EXPECT_EQ(scene.find(entityId)->name, "最终名称");
        operations.beginTransaction(scene, clearColor);
        ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({10, 0, 0})));
        const auto copyId = operations.duplicate(scene, clearColor);
        ASSERT_TRUE(copyId.valid());
        EXPECT_FALSE(operations.inTransaction());
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_EQ(scene.find(copyId), nullptr);
        EXPECT_EQ(scene.find(entityId)->transform.position(), glm::vec3(10, 0, 0));
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_EQ(scene.find(entityId)->transform.position(), glm::vec3(4, 5, 6));

        operations.beginTransaction(scene, clearColor);
        auto modifiedLight = scene.lighting();
        modifiedLight.intensity = 3.5f;
        ASSERT_TRUE(operations.setLighting(scene, clearColor, modifiedLight));
        ASSERT_TRUE(operations.undo(scene, clearColor));
        EXPECT_FALSE(operations.inTransaction());
        EXPECT_FLOAT_EQ(scene.lighting().intensity, 2.5f);
    }

    TEST(EditorOperationsFileTest, SavesLoadsPreservesAssociationOnFailureAndResetsHistory)
    {
        const fs::path sandbox = fs::temp_directory_path() / "incubator_editor_cpu_file_test";
        fs::remove_all(sandbox);
        fs::create_directories(sandbox);
        auto io = FileIO::createPlatformFileIO();

        Editor::EditorOperations operations;
        Scene::Scene scene;
        std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1.0f};
        const auto id = scene.createEntity(nullptr, Scene::Transform({1, 2, 3}), true, "纯CPU实体");
        operations.select(id);
        ASSERT_TRUE(operations.rename(scene, clearColor, "待保存实体"));
        EXPECT_TRUE(operations.isDirty() && operations.canUndo());

        const std::string primary = Utils::Utf8::toUtf8String(sandbox / "primary.json");
        ASSERT_TRUE(operations.save(*io, scene, clearColor, primary));
        EXPECT_TRUE(operations.currentFile() == primary && !operations.isDirty() && !operations.messageError() && operations.canUndo());

        // 保存或加载失败时保持原有文件关联、选中状态与编辑历史。
        ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform({4, 5, 6})));
        EXPECT_FALSE(operations.save(*io, scene, clearColor, Utils::Utf8::toUtf8String(sandbox / "missing-dir/bad.json")));
        EXPECT_TRUE(operations.messageError() && operations.currentFile() == primary && operations.isDirty() && operations.canUndo());

        EXPECT_FALSE(operations.load(*io, Utils::Utf8::toUtf8String(sandbox / "missing.json")).has_value());
        EXPECT_TRUE(operations.messageError() && operations.currentFile() == primary && operations.selected() == id && operations.canUndo());

        // 加载成功切换文件关联并清空编辑历史、选中与脏标记。
        const std::string secondary = Utils::Utf8::toUtf8String(sandbox / "secondary.json");
        {
            Scene::Scene other; other.createEntity(nullptr, Scene::Transform({7, 8, 9}), false, "已加载");
            ASSERT_TRUE(operations.save(*io, other, {0.3f, 0.4f, 0.5f, 1.0f}, secondary));
        }
        ASSERT_TRUE(operations.save(*io, scene, clearColor, primary) && operations.setVisible(scene, clearColor, false));
        const auto loaded = operations.load(*io, secondary);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_TRUE(operations.currentFile() == secondary && !operations.isDirty() && !operations.canUndo() &&
                    !operations.canRedo() && !operations.selected().valid());
        ASSERT_TRUE(loaded->scene->entities().size() == 1u && loaded->scene->entities()[0].name == "已加载");
        fs::remove_all(sandbox);
    }

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

    TEST(EditorControllerTest, UnsavedGuardTransitionsAndFileActionsWorkWithoutWindow)
    {
        Scene::Scene scene;
        std::array<float, 4> clearColor{0.1f, 0.1f, 0.1f, 1.0f};
        Editor::EditorController controller;

        // 干净状态下直接进入 Open / Exit，不触发未保存挂起。
        EXPECT_FALSE(controller.requestOpen(scene, clearColor));
        EXPECT_EQ(controller.fileAction(), Editor::FileAction::Open);
        EXPECT_EQ(controller.pendingAction(), Editor::PendingAction::None);
        controller.cancelFileAction();
        EXPECT_EQ(controller.fileAction(), Editor::FileAction::None);

        EXPECT_TRUE(controller.requestExit(scene, clearColor));
        EXPECT_TRUE(controller.exitRequested());
        controller.clearExitRequested();

        // 产生脏状态后，requestOpen / requestClear / requestExit 均挂起 PendingAction。
        const auto entityId = scene.createEntity();
        controller.select(entityId);
        EXPECT_TRUE(controller.operations().rename(scene, clearColor, "ControllerNode"));
        ASSERT_TRUE(controller.isDirty());

        EXPECT_TRUE(controller.requestOpen(scene, clearColor));
        EXPECT_EQ(controller.pendingAction(), Editor::PendingAction::Open);
        controller.cancelPendingAction();
        EXPECT_EQ(controller.pendingAction(), Editor::PendingAction::None);

        // 未关联文件时点击 Save，转入 pendingSaveAs_ + FileAction::SaveAs。
        EXPECT_TRUE(controller.requestClear(scene, clearColor));
        EXPECT_EQ(controller.pendingAction(), Editor::PendingAction::Clear);
        EXPECT_FALSE(controller.resolveUnsavedSave(scene, clearColor, nullptr));
        EXPECT_TRUE(controller.pendingSaveAs());
        EXPECT_EQ(controller.fileAction(), Editor::FileAction::SaveAs);

        // 取消文件对话框时同步清理挂起状态。
        controller.cancelFileAction();
        controller.cancelPendingAction();
        EXPECT_EQ(controller.fileAction(), Editor::FileAction::None);
        EXPECT_EQ(controller.pendingAction(), Editor::PendingAction::None);

        // 保存到沙盒文件后完成挂起动作（Clear）。
        const auto tempRoot = fs::temp_directory_path() / "incubator_editor_controller_guard_test";
        fs::remove_all(tempRoot);
        fs::create_directories(tempRoot);
        auto fileIO = FileIO::createPlatformFileIO();
        const auto savedPath = (tempRoot / "saved_scene.json").string();
        EXPECT_TRUE(controller.requestClear(scene, clearColor));
        EXPECT_FALSE(controller.resolveUnsavedSave(scene, clearColor, fileIO.get()));
        controller.operationPath(savedPath);
        EXPECT_TRUE(controller.confirmFileAction(scene, clearColor, fileIO.get()));
        EXPECT_EQ(controller.fileAction(), Editor::FileAction::None);
        EXPECT_EQ(controller.pendingAction(), Editor::PendingAction::None);
        EXPECT_TRUE(scene.entities().empty());
        EXPECT_TRUE(controller.isDirty());

        // 脏状态下 Discard 立即执行 Exit。
        scene.createEntity();
        controller.operations().setClearColor(scene, clearColor, {0.3f, 0.2f, 0.1f, 1.0f});
        EXPECT_FALSE(controller.requestExit(scene, clearColor));
        EXPECT_EQ(controller.pendingAction(), Editor::PendingAction::Exit);
        EXPECT_TRUE(controller.discardPendingAction(scene, clearColor));
        EXPECT_TRUE(controller.exitRequested());
        fs::remove_all(tempRoot);
    }

    TEST(EditorControllerTest, PanelTransactionsSwitchCleanlyAndCancelOnFocusLoss)
    {
        Scene::Scene scene;
        std::array<float, 4> clearColor{0.1f, 0.2f, 0.3f, 1.0f};
        Editor::EditorController controller;
        const auto id = scene.createEntity(nullptr, Scene::Transform({1.0f, 0.0f, 0.0f}), true, "PanelTarget");
        controller.select(id);

        // Entity 通道连续修改并切换到 Lighting，自动提交前一条 Entity 历史。
        ASSERT_TRUE(controller.beginPanelTransaction(scene, clearColor, Editor::PanelTransactionKind::Entity));
        EXPECT_EQ(controller.activePanelTransaction(), Editor::PanelTransactionKind::Entity);
        controller.operations().rename(scene, clearColor, "RenamedA");
        controller.operations().rename(scene, clearColor, "RenamedB");

        ASSERT_TRUE(controller.beginPanelTransaction(scene, clearColor, Editor::PanelTransactionKind::Lighting));
        EXPECT_EQ(controller.activePanelTransaction(), Editor::PanelTransactionKind::Lighting);
        EXPECT_TRUE(controller.canUndo());
        EXPECT_EQ(scene.find(id)->name, "RenamedB");

        auto lighting = scene.lighting();
        lighting.intensity = 2.5f;
        controller.operations().setLighting(scene, clearColor, lighting);

        // 切换到 Background 通道，自动提交 Lighting 事务。
        ASSERT_TRUE(controller.beginPanelTransaction(scene, clearColor, Editor::PanelTransactionKind::Background));
        EXPECT_EQ(controller.activePanelTransaction(), Editor::PanelTransactionKind::Background);
        controller.operations().setClearColor(scene, clearColor, {0.8f, 0.7f, 0.6f, 1.0f});

        // 窗口失焦触发 cancelInteraction，应回滚未提交的 Background 事务并保留前两条已提交历史。
        controller.cancelInteraction(scene, clearColor, true, true);
        EXPECT_EQ(controller.activePanelTransaction(), Editor::PanelTransactionKind::None);
        EXPECT_FALSE(controller.operations().inTransaction());
        EXPECT_FLOAT_EQ(clearColor[0], 0.1f);
        EXPECT_FLOAT_EQ(scene.lighting().intensity, 2.5f);

        EXPECT_TRUE(controller.undo(scene, clearColor));
        EXPECT_NE(scene.lighting().intensity, 2.5f);
        EXPECT_TRUE(controller.undo(scene, clearColor));
        EXPECT_EQ(scene.find(id)->name, "PanelTarget");

        // 外部事务激活期间（例如 Gizmo 拖拽），面板不得劫持或误提交该事务。
        controller.operations().beginTransaction(scene, clearColor);
        ASSERT_TRUE(controller.operations().inTransaction());
        EXPECT_FALSE(controller.beginPanelTransaction(scene, clearColor, Editor::PanelTransactionKind::Entity));
        EXPECT_EQ(controller.activePanelTransaction(), Editor::PanelTransactionKind::None);
        controller.commitPanelTransactions(scene, clearColor);
        EXPECT_TRUE(controller.operations().inTransaction());
        controller.operations().cancelTransaction(scene, clearColor);
    }

    TEST(EditorControllerTest, ViewportInputGizmoDragAndOverlayAssemblyWorkWithoutWindow)
    {
        Scene::Scene scene;
        scene.setCamera(Scene::Camera({0.0f, 0.0f, 6.0f}));
        std::array<float, 4> clearColor{0.1f, 0.1f, 0.1f, 1.0f};
        Editor::EditorController controller;

        const auto id = scene.createEntity(nullptr, Scene::Transform({0.0f, 0.0f, 0.0f}), true, "GizmoNode");
        controller.select(id);

        const Render::FrameExtent extent{1280, 720};
        const App::ViewportRegion region{{100.0, 50.0}, {800.0, 600.0}, true, true};
        const auto projected = Editor::projectGizmo(scene, id, Editor::GizmoMode::Translate, extent, region);
        ASSERT_TRUE(projected.visible && !projected.segments.empty());

        // 找到 X 轴线段中点作为悬停和拖拽起点。
        auto xSegIt = std::find_if(projected.segments.begin(), projected.segments.end(),
                                   [](const auto& s) { return s.axis == Editor::GizmoAxis::X; });
        ASSERT_NE(xSegIt, projected.segments.end());
        const glm::vec2 xMidScreen = (xSegIt->segment.start + xSegIt->segment.end) * 0.5f;

        App::UiInput hoverInput{};
        hoverInput.position = {xMidScreen.x - region.origin.x, xMidScreen.y - region.origin.y};
        controller.input(scene, clearColor, extent, region, {1280.0, 720.0}, hoverInput);
        EXPECT_EQ(controller.hoveredGizmoAxis(), Editor::GizmoAxis::X);
        EXPECT_FALSE(controller.gizmoDragging());

        // 无模型实体仅生成 Gizmo 图元，且高亮 X 轴线段排在末尾并以 3.5px 高亮色输出。
        const auto overlay = controller.buildViewportOverlay(scene, extent, region);
        EXPECT_FLOAT_EQ(overlay.clipMin.x, 100.0f);
        EXPECT_FLOAT_EQ(overlay.clipMin.y, 50.0f);
        ASSERT_FALSE(overlay.lines.empty());
        EXPECT_FLOAT_EQ(overlay.lines.back().thickness, 3.5f);
        EXPECT_EQ(overlay.lines.back().color.r, 255);
        EXPECT_EQ(overlay.lines.back().color.g, 240);
        EXPECT_EQ(overlay.lines.back().color.b, 100);

        // 按下开始拖拽，拖拽期间锁定 GizmoMode 切换。
        App::UiInput pressInput = hoverInput;
        pressInput.select = {true, true, false};
        controller.input(scene, clearColor, extent, region, {1280.0, 720.0}, pressInput);
        EXPECT_TRUE(controller.gizmoDragging());
        EXPECT_EQ(controller.activeGizmoAxis(), Editor::GizmoAxis::X);
        controller.setGizmoMode(Editor::GizmoMode::Rotate);
        EXPECT_EQ(controller.gizmoMode(), Editor::GizmoMode::Translate);

        // 拖动并释放提交单步历史。
        App::UiInput dragInput = hoverInput;
        dragInput.position.x += 60.0;
        dragInput.select = {true, false, false};
        controller.input(scene, clearColor, extent, region, {1280.0, 720.0}, dragInput);
        EXPECT_GT(scene.find(id)->transform.position().x, 0.2f);

        App::UiInput releaseInput = dragInput;
        releaseInput.select = {false, false, true};
        controller.input(scene, clearColor, extent, region, {1280.0, 720.0}, releaseInput);
        EXPECT_FALSE(controller.gizmoDragging());
        EXPECT_TRUE(controller.canUndo());
        EXPECT_TRUE(controller.undo(scene, clearColor));
        EXPECT_FLOAT_EQ(scene.find(id)->transform.position().x, 0.0f);
    }

    TEST(EditorControllerTest, RefreshSceneFilesDiscoversSortedJsonAndFailsSilently)
    {
        const auto prevCwd = fs::current_path();
        const auto tempRoot = fs::temp_directory_path() / "incubator_editor_scene_files_test";
        fs::remove_all(tempRoot);
        fs::create_directories(tempRoot);

        struct CwdGuard
        {
            fs::path prev;
            fs::path temp;
            ~CwdGuard()
            {
                std::error_code ec;
                fs::current_path(prev, ec);
                fs::remove_all(temp, ec);
            }
        } guard{prevCwd, tempRoot};

        fs::current_path(tempRoot);
        auto fileIO = FileIO::createPlatformFileIO();
        Editor::EditorController controller;

        // 预设一条非错误消息，确认目录缺失时 refreshSceneFiles 静默返回空且不污染消息或错误标记。
        Scene::Scene scene;
        std::array<float, 4> clearColor{0.1f, 0.1f, 0.1f, 1.0f};
        scene.createEntity();
        ASSERT_TRUE(controller.operations().clear(scene, clearColor));
        const std::string beforeMsg = controller.message();
        ASSERT_FALSE(controller.messageError());

        controller.refreshSceneFiles(*fileIO);
        EXPECT_TRUE(controller.sceneFiles().empty());
        EXPECT_EQ(controller.message(), beforeMsg);
        EXPECT_FALSE(controller.messageError());

        // 创建 assets/scenes/ 下的混合文件（含 UTF-8 中文路径）与子目录，验证仅收集 .json 且按字典序排序。
        fs::create_directories(tempRoot / "assets" / "scenes" / "nested_dir");
        for (const char* rel : {"assets/scenes/z_last.json", "assets/scenes/a_first.json",
                                "assets/scenes/ignore.txt", "assets/scenes/UPPER.JSON",
                                "assets/scenes/中文场景.json"})
        {
            auto handle = fileIO->open(Utils::Utf8::toFsPath(rel),
                                       FileIO::OpenMode::ReadWrite | FileIO::OpenMode::Create |
                                           FileIO::OpenMode::Truncate);
            ASSERT_NE(handle, nullptr);
        }

        controller.refreshSceneFiles(*fileIO);
        const std::vector<std::string> expected = {
            "assets/scenes/UPPER.JSON",
            "assets/scenes/a_first.json",
            "assets/scenes/z_last.json",
            "assets/scenes/中文场景.json",
        };
        EXPECT_EQ(controller.sceneFiles(), expected);
        EXPECT_EQ(controller.message(), beforeMsg);
        EXPECT_FALSE(controller.messageError());
    }

    TEST(EditorControllerTest, SetVisibleByEntityIdDoesNotStealSelectionAndSupportsUndoRedo)
    {
        Scene::Scene scene;
        std::array<float, 4> clearColor{0.1f, 0.1f, 0.1f, 1.0f};
        Editor::EditorController controller;
        const auto first = scene.createEntity(nullptr, Scene::Transform{}, true, "First");
        const auto second = scene.createEntity(nullptr, Scene::Transform{}, true, "Second");
        controller.select(first);

        ASSERT_TRUE(controller.setVisible(scene, clearColor, second, false));
        EXPECT_EQ(controller.selected(), first);
        EXPECT_FALSE(scene.find(second)->visible);
        EXPECT_TRUE(controller.canUndo());
        EXPECT_TRUE(controller.isDirty());

        ASSERT_TRUE(controller.undo(scene, clearColor));
        EXPECT_EQ(controller.selected(), first);
        EXPECT_TRUE(scene.find(second)->visible);

        ASSERT_TRUE(controller.redo(scene, clearColor));
        EXPECT_EQ(controller.selected(), first);
        EXPECT_FALSE(scene.find(second)->visible);
    }

    TEST(EditorControllerTest, AssetBrowserTracksDirectoriesParentEmptyAndErrorsWithoutWindow)
    {
        const auto tempRoot = fs::temp_directory_path() / "incubator_editor_asset_browser_test";
        fs::remove_all(tempRoot);
        fs::create_directories(tempRoot / "assets" / "browser" / "empty");
        auto fileIO = FileIO::createPlatformFileIO();
        struct CleanupGuard
        {
            fs::path temp;
            ~CleanupGuard()
            {
                Resource::ResourceManager::instance().bindAssetSource(nullptr);
                std::error_code ec;
                fs::remove_all(temp, ec);
            }
        } guard{tempRoot};
        ASSERT_NE(fileIO->open(tempRoot / "assets" / "browser" / "sample.obj",
                               FileIO::OpenMode::ReadWrite | FileIO::OpenMode::Create | FileIO::OpenMode::Truncate),
                  nullptr);
        Resource::AssetSource source(*fileIO, tempRoot / "assets");
        Resource::ResourceManager::instance().bindAssetSource(&source);

        Editor::EditorController controller;
        ASSERT_TRUE(controller.browseDirectory("browser"));
        EXPECT_EQ(controller.assetDirectory(), "browser");
        EXPECT_TRUE(controller.assetError().empty());
        ASSERT_EQ(controller.assetEntries().size(), 2u);

        ASSERT_TRUE(controller.browseDirectory("browser/empty"));
        EXPECT_EQ(controller.assetDirectory(), "browser/empty");
        EXPECT_TRUE(controller.assetEntries().empty());
        EXPECT_TRUE(controller.assetError().empty());

        ASSERT_TRUE(controller.parentDirectory());
        EXPECT_EQ(controller.assetDirectory(), "browser");
        EXPECT_TRUE(controller.assetError().empty());

        ASSERT_FALSE(controller.browseDirectory("browser/missing"));
        EXPECT_NE(controller.assetError().find("browser/missing"), std::string::npos);
        EXPECT_TRUE(controller.assetEntries().empty());

        ASSERT_TRUE(controller.parentDirectory());
        EXPECT_TRUE(controller.assetDirectory().empty());
        EXPECT_TRUE(controller.assetError().empty());
        EXPECT_FALSE(controller.assetEntries().empty());
    }

    TEST(EditorControllerTest, ReparentUnparentUndoRedoAndDeleteChildGuardWorkWithoutWindow)
    {
        Scene::Scene scene;
        std::array<float, 4> clearColor{0.1f, 0.1f, 0.1f, 1.0f};
        Editor::EditorController controller;

        const auto parent = scene.createEntity({}, Scene::Transform({2.0f, 0.0f, 0.0f}), true, "Parent");
        const auto child = scene.createEntity({}, Scene::Transform({0.0f, 1.0f, 0.0f}), true, "Child");
        const auto beforeLocal = scene.find(child)->transform.position();

        // 挂接成功记 1 步历史并保持局部变换。
        ASSERT_TRUE(controller.setParent(scene, clearColor, child, parent));
        EXPECT_EQ(scene.find(child)->parent, parent);
        EXPECT_EQ(scene.find(child)->transform.position(), beforeLocal);
        EXPECT_TRUE(controller.canUndo());
        EXPECT_TRUE(controller.isDirty());

        // 重复挂接或循环挂接返回 false，不新增历史。
        EXPECT_FALSE(controller.setParent(scene, clearColor, child, parent));
        EXPECT_FALSE(controller.setParent(scene, clearColor, parent, child));

        // 删除有子级的实体被拒绝，设置错误提示且不新增历史。
        controller.select(parent);
        controller.deleteSelected(scene, clearColor);
        EXPECT_NE(scene.find(parent), nullptr);
        EXPECT_TRUE(controller.messageError());
        EXPECT_NE(controller.message().find("children"), std::string::npos);

        // Undo 恢复为根，Redo 恢复挂接。
        ASSERT_TRUE(controller.undo(scene, clearColor));
        EXPECT_FALSE(scene.find(child)->parent.valid());
        EXPECT_FALSE(controller.canUndo());
        ASSERT_TRUE(controller.redo(scene, clearColor));
        EXPECT_EQ(scene.find(child)->parent, parent);

        // 解除挂接同样可撤销重做。
        ASSERT_TRUE(controller.setParent(scene, clearColor, child, {}));
        EXPECT_FALSE(scene.find(child)->parent.valid());
        ASSERT_TRUE(controller.undo(scene, clearColor));
        EXPECT_EQ(scene.find(child)->parent, parent);
    }

    TEST(EditorControllerTest, ChildWorldAxisTranslateGizmoSolvesUnderRotatedNonUniformParent)
    {
        Scene::Scene scene;
        std::array<float, 4> clearColor{0.1f, 0.1f, 0.1f, 1.0f};
        Editor::EditorController controller;
        scene.setCamera(Scene::Camera({0.0f, 0.0f, 10.0f}));

        const auto parentTx = Scene::Transform(
            {1.0f, 0.0f, 0.0f}, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 1, 0)), {2.0f, 0.5f, 3.0f});
        const auto parent = scene.createEntity({}, parentTx, true, "Parent");
        const auto child = scene.createEntity({}, Scene::Transform({1.0f, 0.0f, 0.0f}), true, "Child");
        ASSERT_TRUE(scene.setParent(child, parent));
        controller.select(child);

        const Render::FrameExtent extent{1280, 800};
        const App::ViewportRegion region{{0.0f, 0.0f}, {1280.0f, 800.0f}};

        // Translate 模式下子实体 Gizmo 锚点在世界位置。
        const auto translateGizmo = Editor::projectGizmo(scene, child, Editor::GizmoMode::Translate, extent, region);
        ASSERT_TRUE(translateGizmo.visible);
        const glm::vec3 initialWorld((*scene.worldMatrix(child))[3]);
        EXPECT_NEAR(glm::length(translateGizmo.origin - initialWorld), 0.0f, 1e-4f);

        // Rotate / Scale 模式下保留 GizmoMode 值但隐藏视口 Gizmo。
        controller.setGizmoMode(Editor::GizmoMode::Rotate);
        EXPECT_EQ(controller.gizmoMode(), Editor::GizmoMode::Rotate);
        EXPECT_FALSE(Editor::projectGizmo(scene, child, controller.gizmoMode(), extent, region).visible);
        controller.setGizmoMode(Editor::GizmoMode::Scale);
        EXPECT_FALSE(Editor::projectGizmo(scene, child, controller.gizmoMode(), extent, region).visible);

        // 父级变换奇异时，Translate 模式也隐藏 Gizmo。
        controller.setGizmoMode(Editor::GizmoMode::Translate);
        ASSERT_TRUE(scene.setTransform(parent, Scene::Transform({1.0f, 0.0f, 0.0f}, {1, 0, 0, 0}, {0.0f, 1.0f, 1.0f})));
        EXPECT_FALSE(Editor::projectGizmo(scene, child, Editor::GizmoMode::Translate, extent, region).visible);

        // 恢复可逆父级并执行真实世界 X 轴平移拖拽，验证世界姿态沿 X 轴移动且局部位移正确反算。
        ASSERT_TRUE(scene.setTransform(parent, parentTx));
        const auto gizmo = Editor::projectGizmo(scene, child, Editor::GizmoMode::Translate, extent, region);
        ASSERT_TRUE(gizmo.visible);
        const auto& xSeg = gizmo.segments[0].segment;
        const glm::vec2 xMid = (xSeg.start + xSeg.end) * 0.5f;

        App::UiInput hover{};
        hover.position = {xMid.x, xMid.y};
        controller.input(scene, clearColor, extent, region, {1280.0, 800.0}, hover);
        EXPECT_EQ(controller.hoveredGizmoAxis(), Editor::GizmoAxis::X);

        App::UiInput press = hover;
        press.select.down = true;
        press.select.pressed = true;
        controller.input(scene, clearColor, extent, region, {1280.0, 800.0}, press);
        ASSERT_TRUE(controller.gizmoDragging());

        App::UiInput drag = hover;
        drag.position = {xMid.x + 60.0, xMid.y};
        drag.select.down = true;
        controller.input(scene, clearColor, extent, region, {1280.0, 800.0}, drag);

        const glm::vec3 movedWorld((*scene.worldMatrix(child))[3]);
        EXPECT_GT(movedWorld.x, initialWorld.x + 0.1f);
        EXPECT_NEAR(movedWorld.y, initialWorld.y, 1e-4f);
        EXPECT_NEAR(movedWorld.z, initialWorld.z, 1e-4f);

        // 失焦取消应回滚到拖拽前局部位置；正常释放则提交 1 步撤销。
        controller.cancelInteraction(scene, clearColor, true, true);
        EXPECT_FALSE(controller.gizmoDragging());
        EXPECT_EQ(scene.find(child)->transform.position(), glm::vec3(1.0f, 0.0f, 0.0f));
        EXPECT_FALSE(controller.canUndo());

        controller.input(scene, clearColor, extent, region, {1280.0, 800.0}, hover);
        controller.input(scene, clearColor, extent, region, {1280.0, 800.0}, press);
        controller.input(scene, clearColor, extent, region, {1280.0, 800.0}, drag);
        App::UiInput release = drag;
        release.select.down = false;
        release.select.released = true;
        controller.input(scene, clearColor, extent, region, {1280.0, 800.0}, release);
        EXPECT_TRUE(controller.canUndo());
        EXPECT_GT(glm::vec3((*scene.worldMatrix(child))[3]).x, initialWorld.x + 0.1f);
        ASSERT_TRUE(controller.undo(scene, clearColor));
        EXPECT_EQ(scene.find(child)->transform.position(), glm::vec3(1.0f, 0.0f, 0.0f));
    }
}  // namespace
