#pragma once

#include <string>
#include <utility>

#include "runtime/scene/scene.h"

namespace Incubator::Editor
{
    /// @brief 按实体标识执行选择和对象编辑，不持有场景或实体指针。
    class EditorSelection
    {
    public:
        Scene::EntityId selected() const noexcept { return selected_; }
        void select(Scene::EntityId id) noexcept { selected_ = id; }
        bool rename(Scene::Scene& scene, std::string name) { return scene.setName(selected_, std::move(name)); }
        bool setVisible(Scene::Scene& scene, bool visible) { return scene.setVisible(selected_, visible); }
        bool setTransform(Scene::Scene& scene, Scene::Transform transform)
        {
            return scene.setTransform(selected_, std::move(transform));
        }
        void remove(Scene::Scene& scene)
        {
            scene.destroyEntity(selected_);
            selected_ = {};
        }
        void clear(Scene::Scene& scene)
        {
            scene.clear();
            selected_ = {};
        }

    private:
        Scene::EntityId selected_;
    };
}  // namespace Incubator::Editor
