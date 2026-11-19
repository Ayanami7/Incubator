#pragma once

#include <span>
#include <vector>

#include "runtime/render/lighting.h"
#include "runtime/scene/camera.h"
#include "runtime/scene/entity.h"

namespace Incubator::Scene
{
    /// @brief 由线段查询得到的实体 ID 和入盒参数；值不持有实体。
    struct PickHit
    {
        EntityId id;
        double parameter;
    };
    /// @brief 扁平、单线程的逻辑场景。
    /// @note 共享 PreparedModel 不会转移 GPU 资源的所有权；ID 操作遇到无效实体时返回 nullptr 或 false，且不影响其他实体。
    class Scene
    {
    public:
        Scene() = default;
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;
        Scene(Scene&&) = delete;
        Scene& operator=(Scene&&) = delete;

        /// @brief 在场景中创建实体。
        /// @param model 实体使用的模型；为空时仍会创建有效的逻辑实体。
        /// @param transform 实体的初始变换。
        /// @param visible 实体的初始可见状态。
        /// @param name 自有 UTF-8 名称；允许空名和重名，不含 NUL。
        /// @return 新实体的场景内标识。实体删除、清空或销毁后，该 ID 不会复用。
        EntityId createEntity(std::shared_ptr<const Model::PreparedModel> model = {}, Transform transform = Transform{},
                              bool visible = true, std::string name = {});

        /// @brief 复制实体的名称、变换、可见性及共享模型，不改变源实体。
        /// @return 新实体 ID；源 ID 无效时返回无效 ID。
        /// @note 结构变化后必须重新获取实体指针和视图；不会加载或准备模型。
        EntityId duplicateEntity(EntityId id);

        /// @brief 按快照顺序完整替换当前场景实体列表并保留原有 EntityId。
        /// @param entities 待恢复的实体快照列表，不可包含无效或重复 EntityId 及非法名称。
        /// @throws Error::Exception 存在无效/重复 ID 或非法 UTF-8/NUL 名称时抛出 CONFIG，且不改变原实体列表。
        /// @note 常规 createEntity/duplicateEntity 不会复用已删除实体 ID；本接口仅用于编辑历史恢复既有实体身份，
        /// 并推进后续 ID 分配器至所有已恢复 ID 之后。结构变化后必须重新获取实体指针和视图。
        void restoreEntities(std::span<const Entity> entities);

        /// @brief 按标识查找实体。
        /// @param id 待查找的实体标识。
        /// @return 找到时返回只读借用指针，否则返回 nullptr。
        const Entity* find(EntityId id) const;
        bool setTransform(EntityId id, Transform transform);
        bool setVisible(EntityId id, bool visible);
        /// @brief 修改名称；无效 ID 返回 false，非法名称抛 CONFIG 并保留原值。
        bool setName(EntityId id, std::string name);
        bool setModel(EntityId id, std::shared_ptr<const Model::PreparedModel> model);
        bool destroyEntity(EntityId id);
        /// @brief 清除实体和模型持有，保留相机与光照。
        void clear();

        /// @brief 按创建顺序返回场景中的有效实体。
        /// @return 只读借用的实体视图。
        /// @note 调用 createEntity、duplicateEntity、destroyEntity 或 clear 后，必须重新获取实体指针或视图；遍历时不得修改实体集合结构。
        std::span<const Entity> entities() const;

        /// @brief 查询可见模型的世界 AABB，最近入盒参数优先；相同参数按创建顺序。
        /// @note 结果是按值返回的粗拾取；ID 在实体删除或场景销毁后失效。
        std::optional<PickHit> pick(const Math::LineSegment& segment) const;
        /// @brief 使用当前相机及图像内归一化位置进行近远裁剪范围拾取。
        std::optional<PickHit> pick(Render::FrameExtent extent, glm::vec2 position) const;

        const Camera& camera() const;
        void setCamera(Camera camera);

        /// @brief 只读借用当前光照值。
        const Render::Lighting& lighting() const;
        /// @brief 整体替换场景光照，保留输入方向的长度。
        /// @throws Error::Exception 光照无效时抛出 CONFIG，且不改变原值。
        void setLighting(Render::Lighting lighting);

    private:
        Entity* findMutable(EntityId id);
        std::vector<Entity> entities_;
        Camera camera_;
        Render::Lighting lighting_{};
    };
}  // namespace Incubator::Scene
