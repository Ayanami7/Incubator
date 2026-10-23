#pragma once

#include <span>
#include <vector>

#include "runtime/scene/camera.h"
#include "runtime/scene/entity.h"

namespace Incubator::Scene
{
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
        /// @return 新实体的场景内标识。实体删除、清空或销毁后，该 ID 不会复用。
        EntityId createEntity(std::shared_ptr<const Model::PreparedModel> model = {}, Transform transform = Transform{},
                              bool visible = true);

        /// @brief 按标识查找实体。
        /// @param id 待查找的实体标识。
        /// @return 找到时返回只读借用指针，否则返回 nullptr。
        const Entity* find(EntityId id) const;
        bool setTransform(EntityId id, Transform transform);
        bool setVisible(EntityId id, bool visible);
        bool setModel(EntityId id, std::shared_ptr<const Model::PreparedModel> model);
        bool destroyEntity(EntityId id);
        void clear();

        /// @brief 按创建顺序返回场景中的有效实体。
        /// @return 只读借用的实体视图。
        /// @note 调用 createEntity、destroyEntity 或 clear 后，必须重新获取实体指针或视图；遍历时不得修改实体集合结构。
        std::span<const Entity> entities() const;

        const Camera& camera() const;
        void setCamera(Camera camera);

    private:
        Entity* findMutable(EntityId id);
        std::vector<Entity> entities_;
        Camera camera_;
    };
}  // namespace Incubator::Scene
