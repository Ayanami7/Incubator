#pragma once

#include <span>
#include <vector>

#include "runtime/scene/camera.h"
#include "runtime/scene/entity.h"

namespace Incubator::Scene
{
    // Flat, single-threaded logical scene. PreparedModel sharing does not transfer GPU ownership.
    class Scene
    {
    public:
        Scene() = default;
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;
        Scene(Scene&&) = delete;
        Scene& operator=(Scene&&) = delete;

        // Null model is a valid logical entity. IDs are never reused after deletion/clear/destruction.
        EntityId createEntity(std::shared_ptr<const Model::PreparedModel> model = {}, Transform transform = Transform{},
                              bool visible = true);

        // Missing/deleted/foreign IDs yield nullptr or false, without affecting other entities.
        const Entity* find(EntityId id) const;
        bool setTransform(EntityId id, Transform transform);
        bool setVisible(EntityId id, bool visible);
        bool setModel(EntityId id, std::shared_ptr<const Model::PreparedModel> model);
        bool destroyEntity(EntityId id);
        void clear();

        // Live entities in creation order. All entity pointers/views must be reacquired after
        // createEntity/destroyEntity/clear; do not structurally mutate while iterating this view.
        std::span<const Entity> entities() const;

        const Camera& camera() const;
        void setCamera(Camera camera);

    private:
        Entity* findMutable(EntityId id);
        std::vector<Entity> entities_;
        Camera camera_;
    };
}  // namespace Incubator::Scene
