#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "app/common/orbit_camera.h"
#include "editor_app.h"
#include "editor_operations.h"
#include "gizmo.h"
#include "runtime/resource/asset_source.h"
#include "workspace.h"

namespace Incubator::Editor
{
    /// @brief 实现 Editor 中的模型场景交互和 UI。
    /// @note 此类只保存使用方状态；实体 Transform、Camera 和光照值由 Runtime Scene 持有。
    class EditorScene
    {
    public:
        void initialize(Editor::EditorApp& app, std::string_view path = "assets/scenes/mixed.json");

        Scene::EntityId createEntity(Editor::EditorApp& app);

        /// @brief 列出虚拟目录；失败显示独立错误，不将访问失败当作空目录。
        bool browseDirectory(std::string_view path = {});
        /// @brief 返回当前虚拟目录；空字符串表示资产根。
        const std::string& assetDirectory() const;
        /// @brief 返回本次目录发现结果；不表示 CPU 加载或 GPU 准备状态。
        const std::vector<Resource::AssetEntry>& assetEntries() const;
        /// @brief 返回本次目录访问错误；成功列出空目录时为空。
        const std::string& assetError() const;
        /// @brief 返回上一级虚拟目录；根目录保持为根。
        bool parentDirectory();

        /// @brief 使用统一预览选项准备模型，成功后才创建并选中实体；失败保留原场景和文件关联。
        bool addModel(Editor::EditorApp& app, std::string_view path,
                      const Scene::Transform& transform = Scene::Transform{});
        /// @brief 返回已准备模型的缓存项数。
        size_t cachedModels() const
        {
            return operations_.cachedModels();
        }
        /// @brief 显式清理未使用的模型缓存。
        size_t collectUnusedModels()
        {
            return operations_.collectUnusedModels();
        }

        void selectModel(bool useObj);

        bool save(Editor::EditorApp& app, std::string_view path);

        bool load(Editor::EditorApp& app, std::string_view path, bool rethrow = false);

        /// @brief 保存到当前文件；没有文件关联时进入另存确认。
        bool save(Editor::EditorApp& app);
        void beginSaveAs();
        void beginOpen();
        void cancelFileAction();
        void requestOpen(Editor::EditorApp& app);
        void requestClear(Editor::EditorApp& app);
        void requestExit(Editor::EditorApp& app);
        void requestClose(Editor::EditorApp& app);
        void operationPath(std::string_view path);
        bool confirmFileAction(Editor::EditorApp& app);
        void layout(Editor::EditorApp& app);
        void restoreLayout();

        bool isDirty() const noexcept
        {
            return operations_.isDirty();
        }
        bool canUndo() const noexcept
        {
            return operations_.canUndo();
        }
        bool canRedo() const noexcept
        {
            return operations_.canRedo();
        }
        bool undo(Editor::EditorApp& app);
        bool redo(Editor::EditorApp& app);
        std::string statusText() const;
        const std::string& message() const;
        /// @brief 获取当前关联场景文件的 UTF-8 路径。
        std::string_view scenePath() const;

        Scene::EntityId selected() const;
        void select(Scene::EntityId id);
        void select(Editor::EditorApp& app, Scene::EntityId id);

        /// @brief 获取当前视口 Gizmo 操作模式。
        GizmoMode gizmoMode() const noexcept;
        /// @brief 设置视口 Gizmo 操作模式；拖拽激活期间调用不打断或篡改正在进行的拖拽模式。
        void setGizmoMode(GizmoMode mode) noexcept;
        /// @brief 获取当前悬停的 Gizmo 坐标轴。
        GizmoAxis hoveredGizmoAxis() const noexcept;
        /// @brief 返回当前是否正在进行 Gizmo 拖拽。
        bool gizmoDragging() const noexcept;
        /// @brief 获取当前正在拖拽的 Gizmo 坐标轴，未拖拽时返回 GizmoAxis::None。
        GizmoAxis activeGizmoAxis() const noexcept;

        void deleteSelected(Editor::EditorApp& app);

        Scene::EntityId duplicateSelected(Editor::EditorApp& app);

        void clear(Editor::EditorApp& app);

        void cancelInteraction(Editor::EditorApp& app);

        bool focusSelected(Editor::EditorApp& app, Render::FrameExtent extent);

        void input(Editor::EditorApp& app, Render::FrameExtent extent, const App::UiInput& input);

        void resetCamera(Editor::EditorApp& app);

        void ui(Editor::EditorApp& app);

    private:
        enum class FileAction
        {
            None,
            Open,
            SaveAs
        };
        enum class PendingAction
        {
            None,
            Open,
            Clear,
            Exit
        };
        void executePendingAction(Editor::EditorApp& app);
        void clearPendingAction() noexcept;
        void clearPanelTransactionFlags() noexcept;
        void commitPanelTransactions(Editor::EditorApp& app);
        void cancelGizmoDrag(Editor::EditorApp& app);
        bool settleForDirtyCheck(Editor::EditorApp& app);
        void handleShortcuts(Editor::EditorApp& app);

        void drawSelectionOverlay(Editor::EditorApp& app) const;

        void assets(Editor::EditorApp& app);

        void menu(Editor::EditorApp& app);

        void statusBar();

        void objects(Editor::EditorApp& app);

        void properties(Editor::EditorApp& app);

        static std::vector<char> pathBuffer(std::string_view path);

        static std::string_view creationPath(bool useObj);

        static Scene::Transform initialTransform(bool useObj);

        FileAction fileAction_ = FileAction::None;
        PendingAction pendingAction_ = PendingAction::None;
        bool pendingSaveAs_ = false;
        Workspace workspace_;
        bool viewportOpen_ = true;
        EditorOperations operations_;
        std::string assetDirectory_;
        std::vector<Resource::AssetEntry> assetEntries_;
        std::string assetError_;
        bool useObj_ = false;
        bool saveRequested_ = false;
        std::vector<char> scenePath_ = pathBuffer("");
        App::OrbitCamera orbit_;
        bool clickCandidate_ = false;
        Render::FrameExtent extent_{0, 0};
        bool entityTransactionActive_ = false;
        bool lightingTransactionActive_ = false;
        bool backgroundTransactionActive_ = false;
        bool shortcutSuspended_ = false;
        GizmoMode gizmoMode_ = GizmoMode::Translate;
        GizmoAxis hoveredGizmoAxis_ = GizmoAxis::None;
        GizmoDragState gizmoDrag_{};
    };

    Editor::Callbacks makeCallbacks(std::string startupScene = "assets/scenes/shadows.json");
}  // namespace Incubator::Editor
