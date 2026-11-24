#include "runtime/scene/scene.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
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
        const auto created = createEntity(copy.model, copy.transform, copy.visible, copy.name);
        if (auto* createdEntity = findMutable(created))
            createdEntity->materialOverrides = copy.materialOverrides;
        if (copy.parent.valid())
            setParent(created, copy.parent);
        return created;
    }

    void Scene::restoreEntities(std::span<const Entity> entities)
    {
        std::unordered_map<uint64_t, size_t> idToIndex;
        idToIndex.reserve(entities.size());
        uint64_t maxId = 0;
        for (size_t i = 0; i < entities.size(); ++i)
        {
            const auto& entity = entities[i];
            if (!entity.id.valid())
                throw Error::Exception(Error::Code::CONFIG, "Entity restore requires valid EntityId");
            if (!validEntityName(entity.name))
                throw Error::Exception(Error::Code::CONFIG, "Entity name must be UTF-8 without NUL");
            if (!idToIndex.emplace(entity.id.value(), i).second)
                throw Error::Exception(Error::Code::CONFIG, "Entity restore requires unique EntityIds");
            if (!entity.materialOverrides.empty())
            {
                if (!entity.model)
                    throw Error::Exception(Error::Code::CONFIG, "Entity restore material override requires a model");
                for (size_t a = 0; a < entity.materialOverrides.size(); ++a)
                {
                    const auto& ov = entity.materialOverrides[a].value;
                    if (!ov.valid())
                        throw Error::Exception(Error::Code::CONFIG, "Entity restore requires valid material override");
                    const auto* slot = entity.model->findMaterialSlot(ov.materialSlot);
                    if (!slot || slot->sourceDesc.shading != Render::ShadingMode::Pbr)
                        throw Error::Exception(Error::Code::CONFIG,
                                               "Entity restore material override requires a PBR model slot");
                    for (size_t b = a + 1; b < entity.materialOverrides.size(); ++b)
                    {
                        if (entity.materialOverrides[b].value.materialSlot == ov.materialSlot)
                            throw Error::Exception(Error::Code::CONFIG,
                                                   "Entity restore cannot contain duplicate material slot overrides");
                    }
                }
            }
            maxId = std::max(maxId, entity.id.value());
        }

        // 在替换前校验整份快照的父级引用、自指与循环，允许父级位于子级之后。
        std::vector<uint8_t> visitState(entities.size(), 0);
        for (size_t i = 0; i < entities.size(); ++i)
        {
            if (visitState[i] == 2)
                continue;
            std::vector<size_t> chain;
            size_t current = i;
            while (visitState[current] == 0)
            {
                visitState[current] = 1;
                chain.push_back(current);
                const EntityId parent = entities[current].parent;
                if (!parent.valid())
                    break;
                if (parent == entities[current].id)
                    throw Error::Exception(Error::Code::CONFIG, "Entity restore cannot parent an entity to itself");
                const auto found = idToIndex.find(parent.value());
                if (found == idToIndex.end())
                    throw Error::Exception(Error::Code::CONFIG, "Entity restore requires existing parent EntityId");
                current = found->second;
                if (visitState[current] == 1)
                    throw Error::Exception(Error::Code::CONFIG, "Entity restore cannot contain parent cycles");
            }
            for (const size_t idx : chain)
                visitState[idx] = 2;
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
        if (entity->model != model)
            entity->materialOverrides.clear();
        entity->model = std::move(model);
        return true;
    }

    bool Scene::setReloadedModel(EntityId id, std::shared_ptr<const Model::PreparedModel> model,
                                 std::vector<EntityMaterialOverride> overrides) noexcept
    {
        auto* entity = findMutable(id);
        if (!entity)
            return false;
        entity->materialOverrides = std::move(overrides);
        entity->model = std::move(model);
        return true;
    }

    bool Scene::setMaterialOverride(EntityId id, MaterialOverride overrideValue,
                                    std::shared_ptr<const Model::PreparedMaterialOverride> compiled)
    {
        auto* entity = findMutable(id);
        if (!entity)
            return false;
        if (!overrideValue.valid())
            throw Error::Exception(Error::Code::CONFIG, "Material override parameters are invalid");
        if (!entity->model)
            throw Error::Exception(Error::Code::CONFIG, "Material override requires an entity model");
        const auto* slot = entity->model->findMaterialSlot(overrideValue.materialSlot);
        if (!slot)
            throw Error::Exception(Error::Code::CONFIG, "Material override slot does not exist on entity model");
        if (slot->sourceDesc.shading != Render::ShadingMode::Pbr)
            throw Error::Exception(Error::Code::CONFIG, "Material override is only supported on PBR slots");

        auto existing = std::ranges::find(entity->materialOverrides, overrideValue.materialSlot,
                                          [](const EntityMaterialOverride& entry) { return entry.value.materialSlot; });
        if (existing != entity->materialOverrides.end())
        {
            existing->value = overrideValue;
            existing->compiled = std::move(compiled);
        }
        else
        {
            entity->materialOverrides.push_back({overrideValue, std::move(compiled)});
            std::ranges::sort(entity->materialOverrides,
                              [](const EntityMaterialOverride& a, const EntityMaterialOverride& b) {
                                  return a.value.materialSlot < b.value.materialSlot;
                              });
        }
        return true;
    }

    bool Scene::clearMaterialOverride(EntityId id, uint32_t materialSlot)
    {
        auto* entity = findMutable(id);
        if (!entity)
            return false;
        return std::erase_if(entity->materialOverrides,
                             [materialSlot](const EntityMaterialOverride& entry) {
                                 return entry.value.materialSlot == materialSlot;
                             }) != 0;
    }

    bool Scene::clearMaterialOverrides(EntityId id)
    {
        auto* entity = findMutable(id);
        if (!entity || entity->materialOverrides.empty())
            return false;
        entity->materialOverrides.clear();
        return true;
    }

    bool Scene::setParent(EntityId id, EntityId parent)
    {
        auto* entity = findMutable(id);
        if (!entity)
            return false;
        if (entity->parent == parent)
            return true;
        if (parent.valid())
        {
            if (parent == id || !find(parent))
                return false;
            EntityId cursor = parent;
            for (size_t steps = 0; cursor.valid() && steps < entities_.size(); ++steps)
            {
                if (cursor == id)
                    return false;
                const auto* ancestor = find(cursor);
                cursor = ancestor ? ancestor->parent : EntityId{};
            }
        }
        entity->parent = parent;
        return true;
    }

    bool Scene::hasChildren(EntityId id) const
    {
        if (!id.valid())
            return false;
        return std::ranges::any_of(entities_, [id](const Entity& entity) { return entity.parent == id; });
    }

    std::optional<glm::mat4> Scene::worldMatrix(EntityId id) const
    {
        const auto* entity = find(id);
        if (!entity)
            return std::nullopt;
        glm::mat4 result = entity->transform.matrix();
        EntityId cursor = entity->parent;
        for (size_t steps = 0; cursor.valid() && steps < entities_.size(); ++steps)
        {
            const auto* parent = find(cursor);
            if (!parent)
                break;
            result = parent->transform.matrix() * result;
            cursor = parent->parent;
        }
        return result;
    }

    bool Scene::destroyEntity(EntityId id)
    {
        if (!find(id) || hasChildren(id))
            return false;
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
            const auto world = worldMatrix(entity.id);
            if (!world)
                continue;
            const auto bounds = entity.model->bounds()->transformed(*world);
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
