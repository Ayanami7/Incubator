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

    TEST(EditorControllerTest, ImportRefreshesAndLocatesModelWithoutChangingDocument)
    {
        const auto tempRoot = fs::temp_directory_path() / "incubator_editor_asset_import_test";
        fs::create_directories(tempRoot / "assets");
        fs::create_directories(tempRoot / "external");
        auto fileIO = FileIO::createPlatformFileIO();
        Resource::AssetSource source(*fileIO, tempRoot / "assets");
        struct CleanupGuard
        {
            fs::path path;
            ~CleanupGuard()
            {
                Resource::ResourceManager::instance().bindAssetSource(nullptr);
                std::error_code error;
                fs::remove_all(path, error);
            }
        } guard{tempRoot};
        Resource::ResourceManager::instance().bindAssetSource(&source);
        const auto external = tempRoot / "external" / "Triangle.OBJ";
        std::ofstream(external) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";

        Editor::EditorController controller;
        const auto first = controller.importExternalModel(source, external);
        ASSERT_TRUE(first) << controller.message();
        EXPECT_EQ(*first, "models/triangle/triangle.obj");
        EXPECT_EQ(controller.assetDirectory(), "models/triangle");
        EXPECT_EQ(controller.focusedAssetPath(), *first);
        ASSERT_EQ(controller.assetEntries().size(), 1u);
        EXPECT_EQ(controller.assetEntries()[0].path, *first);
        EXPECT_EQ(controller.assetEntries()[0].kind, Resource::AssetKind::Model);
        EXPECT_FALSE(controller.messageError());

        ASSERT_TRUE(controller.browseDirectory("models"));
        EXPECT_TRUE(controller.focusedAssetPath().empty());
        const auto second = controller.importExternalModel(source, external);
        ASSERT_TRUE(second);
        EXPECT_EQ(*second, "models/triangle_1/triangle.obj");
        EXPECT_EQ(controller.assetDirectory(), "models/triangle_1");
        EXPECT_EQ(controller.focusedAssetPath(), *second);
        EXPECT_FALSE(controller.importExternalModel(source, tempRoot / "missing.obj"));
        EXPECT_TRUE(controller.messageError());
        EXPECT_NE(controller.message().find("missing.obj"), std::string::npos);
        EXPECT_EQ(controller.assetDirectory(), "models/triangle_1");
        EXPECT_EQ(controller.focusedAssetPath(), *second);
        ASSERT_EQ(controller.assetEntries().size(), 1u);
        EXPECT_EQ(controller.assetEntries()[0].path, *second);
        EXPECT_FALSE(controller.isDirty());
        EXPECT_FALSE(controller.canUndo());
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

    TEST(EditorControllerTest, LocalLightsAddEditRemoveUndoRedoAndPngWriteValidation)
    {
        Scene::Scene scene;
        std::array<float, 4> clearColor{0.1f, 0.1f, 0.1f, 1.0f};
        Editor::EditorController controller;
        auto io = FileIO::createPlatformFileIO();
        const std::string savedPath = "cpu-local-light-undo-test.json";
        std::filesystem::remove(savedPath);
        ASSERT_TRUE(controller.operations().save(*io, scene, clearColor, savedPath));
        EXPECT_FALSE(controller.isDirty());

        ASSERT_TRUE(controller.addLocalLight(scene, clearColor, Render::LocalLightType::Point));
        ASSERT_TRUE(controller.addLocalLight(scene, clearColor, Render::LocalLightType::Spot));
        EXPECT_EQ(scene.lighting().localLights.size(), 2u);
        EXPECT_EQ(scene.lighting().localLights[0].type, Render::LocalLightType::Point);
        EXPECT_EQ(scene.lighting().localLights[1].type, Render::LocalLightType::Spot);
        EXPECT_TRUE(controller.isDirty());

        // 连续面板滑块事务编辑折叠为 1 步撤销。
        ASSERT_TRUE(controller.beginPanelTransaction(scene, clearColor, Editor::PanelTransactionKind::Lighting));
        auto edited = scene.lighting();
        edited.localLights[0].intensity = 14.0f;
        ASSERT_TRUE(controller.operations().setLighting(scene, clearColor, edited));
        edited.localLights[0].intensity = 22.0f;
        ASSERT_TRUE(controller.operations().setLighting(scene, clearColor, edited));
        controller.commitPanelTransactions(scene, clearColor);
        EXPECT_FLOAT_EQ(scene.lighting().localLights[0].intensity, 22.0f);

        ASSERT_TRUE(controller.removeLocalLight(scene, clearColor, 0u));
        EXPECT_EQ(scene.lighting().localLights.size(), 1u);
        EXPECT_EQ(scene.lighting().localLights[0].type, Render::LocalLightType::Spot);

        ASSERT_TRUE(controller.undo(scene, clearColor));
        EXPECT_EQ(scene.lighting().localLights.size(), 2u);
        EXPECT_FLOAT_EQ(scene.lighting().localLights[0].intensity, 22.0f);
        ASSERT_TRUE(controller.undo(scene, clearColor));
        EXPECT_FLOAT_EQ(scene.lighting().localLights[0].intensity, 5.0f);
        ASSERT_TRUE(controller.undo(scene, clearColor));
        EXPECT_EQ(scene.lighting().localLights.size(), 1u);
        ASSERT_TRUE(controller.undo(scene, clearColor));
        EXPECT_TRUE(scene.lighting().localLights.empty());
        EXPECT_FALSE(controller.isDirty());

        ASSERT_TRUE(controller.redo(scene, clearColor));
        EXPECT_EQ(scene.lighting().localLights.size(), 1u);
        EXPECT_TRUE(controller.isDirty());

        // 达到 MAX_LOCAL_LIGHTS = 16 上限后继续添加返回 false。
        for (size_t i = 1; i < Render::MAX_LOCAL_LIGHTS; ++i)
            ASSERT_TRUE(controller.addLocalLight(scene, clearColor, Render::LocalLightType::Point));
        EXPECT_EQ(scene.lighting().localLights.size(), Render::MAX_LOCAL_LIGHTS);
        EXPECT_FALSE(controller.addLocalLight(scene, clearColor, Render::LocalLightType::Point));
        std::filesystem::remove(savedPath);

        // PNG 编码写出基础校验（签名、IHDR/IDAT/IEND 与非法尺寸拦截）。
        const std::filesystem::path pngPath = std::filesystem::temp_directory_path() / "incubator-png-unit-test.png";
        std::filesystem::remove(pngPath);
        const std::array<uint8_t, 16> pixels{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        Editor::writeRgba8Png(*io, pngPath, 2u, 2u, pixels);
        ASSERT_TRUE(std::filesystem::exists(pngPath));
        std::ifstream pngFile(pngPath, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(pngFile)), std::istreambuf_iterator<char>());
        pngFile.close();
        EXPECT_GT(bytes.size(), 40u);
        EXPECT_EQ(static_cast<uint8_t>(bytes[0]), 137u);
        EXPECT_EQ(bytes.substr(1, 3), "PNG");
        EXPECT_NE(bytes.find("IHDR"), std::string::npos);
        EXPECT_NE(bytes.find("IDAT"), std::string::npos);
        EXPECT_NE(bytes.find("IEND"), std::string::npos);
        EXPECT_THROW(Editor::writeRgba8Png(*io, pngPath, 0u, 2u, pixels), Error::Exception);
        EXPECT_THROW(Editor::writeRgba8Png(*io, pngPath, 3u, 2u, pixels), Error::Exception);
        std::filesystem::remove(pngPath);
    }
}  // namespace
