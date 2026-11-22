#include "runtime/scene/scene.h"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/model/prepared_model.h"

namespace Incubator::Scene
{
    namespace
    {
        // 所有 Scene 共用一个计数器。Scene 操作遵循 Runtime 的主线程约定。
        uint64_t gNextEntityId = 1;
    }  // namespace

    EntityId Scene::createEntity(std::shared_ptr<const Model::PreparedModel> model, Transform transform, bool visible,
                                 std::string name)
    {
        if (!validEntityName(name))
            throw Error::Exception(Error::Code::CONFIG, "Entity name must be UTF-8 without NUL");
        if (gNextEntityId == 0)
            throw Error::Exception(Error::Code::CONFIG, "EntityId space is exhausted");
        const EntityId id(gNextEntityId++);
        entities_.push_back({id, std::move(transform), visible, std::move(model), std::move(name)});
        return id;
    }

    EntityId Scene::duplicateEntity(EntityId id)
    {
        const auto* entity = find(id);
        if (!entity)
            return {};
        const auto copy = *entity;
        return createEntity(copy.model, copy.transform, copy.visible, copy.name);
    }

    void Scene::restoreEntities(std::span<const Entity> entities)
    {
        std::unordered_set<uint64_t> seenIds;
        seenIds.reserve(entities.size());
        uint64_t maxId = 0;
        for (const auto& entity : entities)
        {
            if (!entity.id.valid())
                throw Error::Exception(Error::Code::CONFIG, "Entity restore requires valid EntityId");
            if (!validEntityName(entity.name))
                throw Error::Exception(Error::Code::CONFIG, "Entity name must be UTF-8 without NUL");
            if (!seenIds.insert(entity.id.value()).second)
                throw Error::Exception(Error::Code::CONFIG, "Entity restore requires unique EntityIds");
            maxId = std::max(maxId, entity.id.value());
        }

        std::vector<Entity> restored(entities.begin(), entities.end());
        entities_ = std::move(restored);
        if (maxId > 0 && (gNextEntityId == 0 || maxId >= gNextEntityId))
        {
            gNextEntityId = (maxId == std::numeric_limits<uint64_t>::max()) ? 0 : (maxId + 1);
        }
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

    bool Scene::setName(EntityId id, std::string name)
    {
        auto* entity = findMutable(id);
        if (!entity)
            return false;
        if (!validEntityName(name))
            throw Error::Exception(Error::Code::CONFIG, "Entity name must be UTF-8 without NUL");
        entity->name = std::move(name);
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

    std::optional<PickHit> Scene::pick(const Math::LineSegment& segment) const
    {
        std::optional<PickHit> nearest;
        for (const auto& entity : entities_)
        {
            if (!entity.visible || !entity.model || !entity.model->bounds())
                continue;
            const auto bounds = entity.model->bounds()->transformed(entity.transform.matrix());
            if (!bounds)
                continue;
            const auto hit = Math::intersectSegment(segment, *bounds);
            if (hit && (!nearest || *hit < nearest->parameter))
                nearest = PickHit{entity.id, *hit};
        }
        return nearest;
    }

    std::optional<PickHit> Scene::pick(Render::FrameExtent extent, glm::vec2 position) const
    {
        const auto segment = camera_.viewportSegment(extent, position);
        return segment ? pick(*segment) : std::nullopt;
    }

    const Camera& Scene::camera() const
    {
        return camera_;
    }

    void Scene::setCamera(Camera camera)
    {
        camera_ = std::move(camera);
    }

    const Render::Lighting& Scene::lighting() const
    {
        return lighting_;
    }

    void Scene::setLighting(Render::Lighting lighting)
    {
        if (!lighting.valid())
            throw Error::Exception(Error::Code::CONFIG,
                                   "Scene lighting requires a finite nonzero direction, RGB and "
                                   "ambient in [0, 1], and nonnegative finite intensity");
        lighting_ = lighting;
    }

    const Render::DisplaySettings& Scene::display() const
    {
        return display_;
    }

    void Scene::setDisplay(Render::DisplaySettings display)
    {
        if (!display.valid())
            throw Error::Exception(Error::Code::CONFIG,
                                   "Scene display requires valid tone mapping mode and finite exposureEv in [-8, 8]");
        display_ = display;
    }
}  // namespace Incubator::Scene
