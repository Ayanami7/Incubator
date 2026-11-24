#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/common/orbit_camera.h"
#include "app/common/viewport_input.h"
#include "editor_operations.h"
#include "gizmo.h"
#include "runtime/platform/input/input_state.h"
#include "runtime/render/frame_input.h"
#include "runtime/resource/asset_source.h"
#include "runtime/scene/entity.h"
#include "runtime/scene/transform.h"
#include "selection_overlay.h"

namespace Incubator::FileIO
{
    class FileIO;
}

namespace Incubator::Render
{
    class Renderer;
}

namespace Incubator::Scene
{
    class Scene;
}

namespace Incubator::Editor
{
    class EditorApp;

    /// @brief 当前待确认的文件对话框动作类型。
    enum class FileAction
    {
        None,
        Open,
        SaveAs
    };

    /// @brief 因未保存修改而被拦截挂起的文档动作类型。
    enum class PendingAction
    {
        None,
        Open,
        Clear,
        Exit
    };

    /// @brief Properties 面板连续属性编辑的互斥事务通道类型。
    enum class PanelTransactionKind
    {
        None,
        Entity,
        Material,
        Lighting,
        Display,
        Background
    };

    /// @brief 不依赖 ImGui 的窗口级快捷键状态快照。
    struct ShortcutInput
    {
        bool focused = false;
        bool focusLost = false;
        bool inputQueueEmpty = true;
        bool wantTextInput = false;
        bool popupOpen = false;
        bool keyAlt = false;
        bool keySuper = false;
        bool keyCtrl = false;
        bool keyShift = false;
        bool mouseRightDown = false;
        bool mouseMiddleDown = false;
        bool wDown = false;
        bool eDown = false;
        bool rDown = false;
        bool zDown = false;
        bool yDown = false;
        bool wPressed = false;
        bool ePressed = false;
        bool rPressed = false;
        bool zPressed = false;
        bool yPressed = false;
    };

    /// @brief 零 ImGui 依赖的编辑器纯 C++ 控制器。
    /// @note 统一承载文档工作流、未保存拦截、虚拟资产浏览、连续事务通道、视口 Gizmo/相机状态机与叠加图元组装。
    class EditorController
    {
    public:
        /// @brief 初始化控制器关联的渲染器并在指定场景路径非空时加载初始场景。
        void initialize(EditorApp& app, std::string_view path = "assets/scenes/mixed.json");

        /// @brief 仅绑定渲染器（用于无窗口/测试场景下初始化底层操作）。
        void initialize(Render::Renderer& renderer);

        /// @brief 获取底层编辑操作与历史管理器。
        EditorOperations& operations() noexcept
        {
            return operations_;
        }

        /// @brief 获取底层编辑操作与历史管理器（只读）。
        const EditorOperations& operations() const noexcept
        {
            return operations_;
        }

        /// @brief 使用默认测试模型快捷创建实体。
        Scene::EntityId createEntity(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        Scene::EntityId createEntity(EditorApp& app);

        /// @brief 列出虚拟目录；失败显示独立错误，不将访问失败当作空目录。
        bool browseDirectory(std::string_view path = {});
        /// @brief 返回当前虚拟目录；空字符串表示资产根。
        const std::string& assetDirectory() const noexcept
        {
            return assetDirectory_;
        }
        /// @brief 返回本次目录发现结果；不表示 CPU 加载或 GPU 准备状态。
        const std::vector<Resource::AssetEntry>& assetEntries() const noexcept
        {
            return assetEntries_;
        }
        /// @brief 返回本次目录访问错误；成功列出空目录时为空。
        const std::string& assetError() const noexcept
        {
            return assetError_;
        }
        /// @brief 返回上一级虚拟目录；根目录保持为根。
        bool parentDirectory();

        /// @brief 导入外部模型，刷新并定位新模型，返回此次导入的虚拟路径。
        std::optional<std::string> importExternalModel(Resource::AssetSource& assetSource,
                                                       const std::filesystem::path& externalPath);
        std::optional<std::string> importExternalModel(EditorApp& app, const std::filesystem::path& externalPath);
        /// @brief 返回当前目录中最近导入并定位的模型路径，空串表示没有定位项。
        const std::string& focusedAssetPath() const noexcept
        {
            return focusedAssetPath_;
        }
        /// @brief 显式重载此次会话已缓存的模型变体，返回操作是否成功。
        bool reloadAsset(Scene::Scene& scene, std::string_view virtualPath);
        bool reloadAsset(EditorApp& app, std::string_view virtualPath);

        /// @brief 使用统一预览选项准备模型，成功后才创建并选中实体；失败保留原场景和文件关联。
        bool addModel(Scene::Scene& scene, const std::array<float, 4>& clearColor, std::string_view path,
                      const Scene::Transform& transform = Scene::Transform{});
        bool addModel(EditorApp& app, std::string_view path, const Scene::Transform& transform = Scene::Transform{});

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

        void selectModel(bool useObj) noexcept
        {
            useObj_ = useObj;
        }
        bool useObjModel() const noexcept
        {
            return useObj_;
        }

        /// @brief 将当前场景保存到指定路径。
        bool save(const Scene::Scene& scene, const std::array<float, 4>& clearColor, FileIO::FileIO* fileIO,
                  std::string_view path);
        bool save(EditorApp& app, std::string_view path);

        /// @brief 保存到当前关联文件；没有文件关联时转入另存确认。
        bool save(const Scene::Scene& scene, const std::array<float, 4>& clearColor, FileIO::FileIO* fileIO);
        bool save(EditorApp& app);

        /// @brief 从指定路径加载场景并同步相机与控制器状态。
        bool load(std::unique_ptr<Scene::Scene>& scene, std::array<float, 4>& clearColor, FileIO::FileIO* fileIO,
                  std::string_view path, bool rethrow = false);
        bool load(EditorApp& app, std::string_view path, bool rethrow = false);

        void beginSaveAs() noexcept;
        void beginOpen() noexcept;
        void cancelFileAction() noexcept;

        FileAction fileAction() const noexcept
        {
            return fileAction_;
        }
        PendingAction pendingAction() const noexcept
        {
            return pendingAction_;
        }
        bool pendingSaveAs() const noexcept
        {
            return pendingSaveAs_;
        }
        bool exitRequested() const noexcept
        {
            return exitRequested_;
        }
        void clearExitRequested() noexcept
        {
            exitRequested_ = false;
        }

        void requestSave() noexcept
        {
            saveRequested_ = true;
        }
        bool consumeSaveRequested() noexcept;

        /// @brief 请求打开场景；若当前存在未保存修改则挂起 PendingAction::Open。
        bool requestOpen(Scene::Scene& scene, std::array<float, 4>& clearColor);
        void requestOpen(EditorApp& app);

        /// @brief 请求清空场景；若当前存在未保存修改则挂起 PendingAction::Clear。
        bool requestClear(Scene::Scene& scene, std::array<float, 4>& clearColor);
        void requestClear(EditorApp& app);

        /// @brief 请求退出编辑器；无未保存修改时置位 exitRequested_，否则挂起 PendingAction::Exit。
        bool requestExit(Scene::Scene& scene, std::array<float, 4>& clearColor);
        void requestExit(EditorApp& app);

        /// @brief 响应窗口关闭请求；存在未保存修改时返回 true（需阻止关闭）并挂起 PendingAction::Exit。
        bool requestClose(Scene::Scene& scene, std::array<float, 4>& clearColor);
        void requestClose(EditorApp& app);

        /// @brief 在未保存确认弹窗中点击 Save：已有路径时直接保存并继续挂起动作，否则转入 SaveAs。
        bool resolveUnsavedSave(Scene::Scene& scene, std::array<float, 4>& clearColor, FileIO::FileIO* fileIO);
        bool resolveUnsavedSave(EditorApp& app);

        /// @brief 在未保存确认弹窗中点击 Discard：直接执行挂起动作并清理挂起状态。
        bool discardPendingAction(Scene::Scene& scene, std::array<float, 4>& clearColor);
        bool discardPendingAction(EditorApp& app);

        /// @brief 取消未保存确认流程并清理挂起状态。
        void cancelPendingAction() noexcept;

        void operationPath(std::string_view path);
        std::vector<char>& pathBufferData() noexcept
        {
            return scenePath_;
        }
        const std::vector<char>& pathBufferData() const noexcept
        {
            return scenePath_;
        }

        /// @brief 扫描 assets/scenes 及当前关联场景所在目录下的 .json 文件，目录缺失或异常时静默返回空列表。
        void refreshSceneFiles(FileIO::FileIO& fileIO);
        void refreshSceneFiles(EditorApp& app);
        /// @brief 返回最近一次扫描并去重排序后的规范化相对场景路径列表。
        const std::vector<std::string>& sceneFiles() const noexcept
        {
            return sceneFiles_;
        }

        /// @brief 确认当前文件对话框动作（Open 或 SaveAs），并在必要时完成挂起流程。
        bool confirmFileAction(std::unique_ptr<Scene::Scene>& scene, std::array<float, 4>& clearColor,
                               FileIO::FileIO* fileIO);
        bool confirmFileAction(Scene::Scene& scene, std::array<float, 4>& clearColor, FileIO::FileIO* fileIO);
        bool confirmFileAction(EditorApp& app);

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
        bool undo(Scene::Scene& scene, std::array<float, 4>& clearColor);
        bool undo(EditorApp& app);
        bool redo(Scene::Scene& scene, std::array<float, 4>& clearColor);
        bool redo(EditorApp& app);

        std::string statusText() const;
        const std::string& message() const noexcept
        {
            return operations_.message();
        }
        bool messageError() const noexcept
        {
            return operations_.messageError();
        }
        std::string_view scenePath() const noexcept
        {
            return operations_.currentFile();
        }

        Scene::EntityId selected() const noexcept
        {
            return operations_.selected();
        }
        void select(Scene::EntityId id);
        void select(Scene::Scene& scene, std::array<float, 4>& clearColor, Scene::EntityId id);
        void select(EditorApp& app, Scene::EntityId id);

        GizmoMode gizmoMode() const noexcept
        {
            return gizmoMode_;
        }
        void setGizmoMode(GizmoMode mode) noexcept;
        GizmoAxis hoveredGizmoAxis() const noexcept
        {
            return hoveredGizmoAxis_;
        }
        bool gizmoDragging() const noexcept
        {
            return gizmoDrag_.active;
        }
        GizmoAxis activeGizmoAxis() const noexcept
        {
            return gizmoDrag_.active ? gizmoDrag_.axis : GizmoAxis::None;
        }

        void deleteSelected(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        void deleteSelected(EditorApp& app);

        bool setVisible(Scene::Scene& scene, const std::array<float, 4>& clearColor, Scene::EntityId id, bool visible);
        bool setVisible(EditorApp& app, Scene::EntityId id, bool visible);
        /// @brief 先结算 Gizmo 与面板事务，再设置或解除实体的父级（保留局部变换）。
        bool setParent(Scene::Scene& scene, std::array<float, 4>& clearColor, Scene::EntityId id,
                       Scene::EntityId parent);
        bool setParent(EditorApp& app, Scene::EntityId id, Scene::EntityId parent);
        /// @brief 先结算 Gizmo 与面板事务，再为选中实体切换模型准备着色模式。
        bool setEntityShading(Scene::Scene& scene, std::array<float, 4>& clearColor, Render::ShadingMode shading);
        bool setEntityShading(EditorApp& app, Render::ShadingMode shading);
        /// @brief 先结算 Gizmo 与其他面板事务，再为选中实体设置 PBR 材质槽覆盖。
        bool setMaterialOverride(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                 const Scene::MaterialOverride& overrideValue);
        bool setMaterialOverride(EditorApp& app, const Scene::MaterialOverride& overrideValue);
        /// @brief 先结算 Gizmo 与面板事务，再重置选中实体指定源材质槽的覆盖。
        bool resetMaterialOverride(Scene::Scene& scene, std::array<float, 4>& clearColor, uint32_t materialSlot);
        bool resetMaterialOverride(EditorApp& app, uint32_t materialSlot);
        /// @brief 先结算 Gizmo 与面板事务，再清除选中实体全部材质槽的覆盖。
        bool resetAllMaterialOverrides(Scene::Scene& scene, std::array<float, 4>& clearColor);
        bool resetAllMaterialOverrides(EditorApp& app);
        /// @brief 先结算 Gizmo 与面板事务，再更新场景显示（色调映射与曝光）设置并记一步历史。
        bool setDisplay(Scene::Scene& scene, std::array<float, 4>& clearColor, Render::DisplaySettings display);
        bool setDisplay(EditorApp& app, Render::DisplaySettings display);

        Scene::EntityId duplicateSelected(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        Scene::EntityId duplicateSelected(EditorApp& app);

        void clear(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        void clear(EditorApp& app);

        /// @brief 开启指定面板属性通道的连续事务；若已有其他通道事务激活则先自动提交。
        bool beginPanelTransaction(Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                   PanelTransactionKind kind);
        bool beginPanelTransaction(EditorApp& app, PanelTransactionKind kind);

        /// @brief 提交当前激活的面板属性事务（若存在）。
        void commitPanelTransactions(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        void commitPanelTransactions(EditorApp& app);

        /// @brief 取消并回滚当前激活的面板属性事务（若存在）。
        void cancelPanelTransactions(Scene::Scene& scene, std::array<float, 4>& clearColor);
        void cancelPanelTransactions(EditorApp& app);

        /// @brief 若底层已无进行中事务，则同步复位面板通道标记。
        void syncPanelTransactionState() noexcept;

        PanelTransactionKind activePanelTransaction() const noexcept
        {
            return activePanelTransaction_;
        }

        void cancelInteraction(Scene::Scene& scene, std::array<float, 4>& clearColor, bool windowUnavailable,
                               bool viewportVisible);
        void cancelInteraction(EditorApp& app);

        bool focusSelected(Scene::Scene& scene, Render::FrameExtent extent);
        bool focusSelected(EditorApp& app, Render::FrameExtent extent);

        void input(Scene::Scene& scene, std::array<float, 4>& clearColor, Render::FrameExtent extent,
                   const App::ViewportRegion& region, Input::PointerValue windowSize, const App::UiInput& uiInput,
                   float dragThreshold = 6.0f);
        void input(EditorApp& app, Render::FrameExtent extent, const App::UiInput& uiInput, float dragThreshold = 6.0f);

        void resetCamera(Scene::Scene& scene);
        void resetCamera(EditorApp& app);

        void handleShortcuts(Scene::Scene& scene, std::array<float, 4>& clearColor, const ShortcutInput& state);
        void handleShortcuts(EditorApp& app, const ShortcutInput& state);

        /// @brief 组装纯 C++ 视口叠加辅助线段批次（先 AABB 框线、再非高亮 Gizmo 轴、最后高亮轴）。
        ViewportOverlay buildViewportOverlay(const Scene::Scene& scene, Render::FrameExtent extent,
                                             const App::ViewportRegion& region) const;
        ViewportOverlay buildViewportOverlay(EditorApp& app) const;

        Render::FrameExtent lastExtent() const noexcept
        {
            return extent_;
        }
        /// @brief 在视口布局更新后同步有效渲染尺寸，避免切换场景或失焦恢复后按钮聚焦失效。
        void syncViewportExtent(Render::FrameExtent extent) noexcept;

        static std::vector<char> pathBuffer(std::string_view path);

    private:
        void executePendingAction(Scene::Scene& scene, std::array<float, 4>& clearColor);
        void cancelGizmoDrag(Scene::Scene& scene, std::array<float, 4>& clearColor);
        bool settleForDirtyCheck(Scene::Scene& scene, std::array<float, 4>& clearColor);

        static std::string_view creationPath(bool useObj);
        static Scene::Transform initialTransform(bool useObj);

        FileAction fileAction_ = FileAction::None;
        PendingAction pendingAction_ = PendingAction::None;
        bool pendingSaveAs_ = false;
        bool exitRequested_ = false;
        EditorOperations operations_;
        bool browseDirectory(std::string_view path, const Resource::AssetSource* source);
        std::string assetDirectory_;
        std::string focusedAssetPath_;
        std::vector<Resource::AssetEntry> assetEntries_;
        std::string assetError_;
        bool useObj_ = false;
        bool saveRequested_ = false;
        std::vector<char> scenePath_ = pathBuffer("");
        std::vector<std::string> sceneFiles_;
        App::OrbitCamera orbit_;
        bool clickCandidate_ = false;
        Render::FrameExtent extent_{0, 0};
        PanelTransactionKind activePanelTransaction_ = PanelTransactionKind::None;
        bool shortcutSuspended_ = false;
        GizmoMode gizmoMode_ = GizmoMode::Translate;
        GizmoAxis hoveredGizmoAxis_ = GizmoAxis::None;
        GizmoDragState gizmoDrag_{};
    };
}  // namespace Incubator::Editor
