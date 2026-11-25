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

    TEST(EditorOperationsTest, ReloadWithoutCachedModelsRespectsActiveTransaction)
    {
        Editor::EditorOperations operations;
        Scene::Scene scene;
        const std::array<float, 4> clearColor{0, 0, 0, 1};
        operations.beginTransaction(scene, clearColor);
        EXPECT_FALSE(operations.reloadModel(scene, "models/model.gltf"));
        EXPECT_TRUE(operations.messageError());
        EXPECT_TRUE(operations.inTransaction());
        EXPECT_FALSE(operations.commitTransaction(scene, clearColor));

        const auto result = operations.reloadModel(scene, "models/model.gltf");
        ASSERT_TRUE(result);
        EXPECT_EQ(result->reloadedVariants, 0u);
        EXPECT_EQ(result->updatedEntities, 0u);
        EXPECT_EQ(result->updatedHistorySnapshots, 0u);
        EXPECT_FALSE(operations.messageError());
        EXPECT_FALSE(operations.isDirty());
        EXPECT_FALSE(operations.canUndo());
    }
}  // namespace
