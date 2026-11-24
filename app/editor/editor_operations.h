#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor_selection.h"
#include "runtime/model/model_manager.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/render/display.h"
#include "runtime/render/lighting.h"
#include "runtime/scene/scene_rebuild.h"

namespace Incubator::Editor
{
    /// @brief 组合选择状态并执行场景编辑与文件操作，维护文档脏状态与编辑历史，不持有场景或实体指针。
    class EditorOperations
    {
    public:
        static constexpr size_t MAX_HISTORY = 64;

        void initialize(Render::Renderer& renderer)
        {
            renderer_ = &renderer;
            cache_ = std::make_unique<Model::ModelManager>(renderer);
        }

        Scene::EntityId selected() const noexcept
        {
            return selection_.selected();
        }
        void select(Scene::EntityId id) noexcept
        {
            selection_.select(id);
        }

        bool addModel(Scene::Scene& scene, const std::array<float, 4>& clearColor, std::string_view path,
                      const Scene::Transform& transform = Scene::Transform{});
        bool rename(Scene::Scene& scene, const std::array<float, 4>& clearColor, std::string name);
        bool setVisible(Scene::Scene& scene, const std::array<float, 4>& clearColor, bool visible);
        bool setVisible(Scene::Scene& scene, const std::array<float, 4>& clearColor, Scene::EntityId id, bool visible);
        /// @brief 为选中实体切换模型准备着色模式，成功替换后压入一条撤销记录；失败保留原模型与历史。
        bool setEntityShading(Scene::Scene& scene, const std::array<float, 4>& clearColor, Render::ShadingMode shading);
        /// @brief 为选中实体指定源材质槽设置 PBR 覆盖，按需构建独立 GPU 材质并支持连续事务或单步历史。
        bool setMaterialOverride(Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                 const Scene::MaterialOverride& overrideValue);
        /// @brief 重置选中实体指定源材质槽的覆盖并恢复源材质，压入一条撤销记录。
        bool resetMaterialOverride(Scene::Scene& scene, const std::array<float, 4>& clearColor, uint32_t materialSlot);
        /// @brief 清除选中实体全部材质槽覆盖并恢复源材质，压入一条撤销记录。
        bool resetAllMaterialOverrides(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        /// @brief 设置或解除实体父级（保留局部变换）；成功变更时压入一条撤销记录，无变化或拒绝不入栈。
        bool setParent(Scene::Scene& scene, const std::array<float, 4>& clearColor, Scene::EntityId id,
                       Scene::EntityId parent);
        bool setTransform(Scene::Scene& scene, const std::array<float, 4>& clearColor, Scene::Transform transform);
        Scene::EntityId duplicate(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        bool remove(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        bool clear(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        bool setLighting(Scene::Scene& scene, const std::array<float, 4>& clearColor, Render::Lighting lighting);
        bool setDisplay(Scene::Scene& scene, const std::array<float, 4>& clearColor, Render::DisplaySettings display);
        bool setClearColor(const Scene::Scene& scene, std::array<float, 4>& clearColor,
                           const std::array<float, 4>& newColor);

        bool canUndo() const noexcept
        {
            return !undoStack_.empty();
        }
        bool canRedo() const noexcept
        {
            return !redoStack_.empty();
        }
        /// @brief 当前是否处于连续编辑事务中。
        bool inTransaction() const noexcept
        {
            return transaction_.has_value();
        }
        /// @brief 开始一次连续编辑事务，采集起始场景与背景色快照；若已在事务中则保持当前起始快照。
        void beginTransaction(const Scene::Scene& scene, const std::array<float, 4>& clearColor);
        /// @brief 提交当前连续编辑事务；若实体、光照或背景色相对起始快照发生变化则压入 1 条撤销记录，未变化则静默丢弃。
        bool commitTransaction(const Scene::Scene& scene, const std::array<float, 4>& clearColor);
        /// @brief 取消当前连续编辑事务并回滚场景与背景色到起始快照，不产生历史记录且不修改 revision。
        bool cancelTransaction(Scene::Scene& scene, std::array<float, 4>& clearColor);
        /// @brief 撤销最近一次历史记录并还原 Scene、clearColor 与选中标识。
        bool undo(Scene::Scene& scene, std::array<float, 4>& clearColor);
        /// @brief 重做最近一次被撤销的记录并还原 Scene、clearColor 与选中标识。
        bool redo(Scene::Scene& scene, std::array<float, 4>& clearColor);

        bool save(FileIO::FileIO& file, const Scene::Scene& scene, const std::array<float, 4>& color,
                  std::string_view path);

        /// @brief 重建成功后返回新场景，清空编辑历史并重置脏状态基线；失败保留原历史、选择及文件关联。
        std::optional<Scene::RebuiltScene> load(FileIO::FileIO& file, std::string_view path, bool rethrow = false);

        bool isDirty() const noexcept
        {
            return !savedRevision_.has_value() || currentRevision_ != *savedRevision_;
        }
        size_t cachedModels() const
        {
            return cache_->size();
        }
        size_t collectUnusedModels();
        const std::string& currentFile() const
        {
            return currentFile_;
        }
        const std::string& message() const
        {
            return message_;
        }
        bool messageError() const
        {
            return messageError_;
        }

    private:
        struct HistorySnapshot
        {
            std::vector<Scene::Entity> entities;
            Render::Lighting lighting{};
            Render::DisplaySettings display{};
            std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1.0f};
            Scene::EntityId selected;
            uint64_t revision = 0;
        };

        HistorySnapshot captureSnapshot(const Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                        uint64_t revision) const;
        void pushUndoSnapshot(HistorySnapshot before);
        void applySnapshot(Scene::Scene& scene, std::array<float, 4>& clearColor, const HistorySnapshot& snapshot);
        static std::string logPath(std::string_view path);

        static constexpr Model::PrepareOptions PREVIEW_OPTIONS{
            Render::TextureMipMode::FullChain,
            {Render::TextureFilter::Linear, Render::TextureFilter::Linear, Render::MipFilter::Linear},
            Render::ShadingMode::Lambert};
        EditorSelection selection_;
        Render::Renderer* renderer_ = nullptr;
        std::unique_ptr<Model::ModelManager> cache_;
        std::string currentFile_;
        std::string message_;
        bool messageError_ = false;
        std::vector<HistorySnapshot> undoStack_;
        std::vector<HistorySnapshot> redoStack_;
        std::optional<HistorySnapshot> transaction_;
        uint64_t currentRevision_ = 0;
        uint64_t nextRevision_ = 1;
        std::optional<uint64_t> savedRevision_ = 0;
    };
}  // namespace Incubator::Editor
