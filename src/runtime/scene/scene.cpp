#include "runtime/scene/scene.h"

#include <algorithm>
#include <utility>

#include "runtime/core/error/error.h"

namespace Incubator::Scene
{
    EntityId Scene::createEntity(std::shared_ptr<const Model::PreparedModel> model, Transform transform, bool visible)
    {
        // One counter across all Scenes. Scene operations follow the Runtime main-thread contract.
        static uint64_t nextId = 1;
        if (nextId == 0)
            throw Error::Exception(Error::Code::CONFIG, "EntityId space is exhausted");
        const EntityId id(nextId++);
        entities_.push_back({id, std::move(transform), visible, std::move(model)});
        return id;
    }

    const Entity* Scene::find(EntityId id) const
    {
        const auto found = std::ranges::find(entities_, id, &Entity::id);
        return found == entities_.end() ? nullptr : &*found;
    }

    Entity* Scene::findMutable(EntityId id)
    {
        const auto found = std::ranges::find(entities_, id, &Entity::id);
        return found == entities_.end() ? nullptr : &*found;
    }

    bool Scene::setTransform(EntityId id, Transform transform)
    {
        auto* entity = findMutable(id);
        if (!entity)
            return false;
        entity->transform = std::move(transform);
        return true;
    }

    bool Scene::setVisible(EntityId id, bool visible)
    {
        auto* entity = findMutable(id);
        if (!entity)
            return false;
        entity->visible = visible;
        return true;
    }

    bool Scene::setModel(EntityId id, std::shared_ptr<const Model::PreparedModel> model)
    {
        auto* entity = findMutable(id);
        if (!entity)
            return false;
        entity->model = std::move(model);
        return true;
    }

    bool Scene::destroyEntity(EntityId id)
    {
        return std::erase_if(entities_, [id](const Entity& entity) { return entity.id == id; }) != 0;
    }

    void Scene::clear()
    {
        entities_.clear();
    }

    std::span<const Entity> Scene::entities() const
    {
        return entities_;
    }

    const Camera& Scene::camera() const
    {
        return camera_;
    }

    void Scene::setCamera(Camera camera)
    {
        camera_ = std::move(camera);
    }
}  // namespace Incubator::Scene
