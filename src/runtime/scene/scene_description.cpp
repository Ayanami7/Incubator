#include "runtime/scene/scene_description.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/model/prepared_model.h"
#include "runtime/scene/scene.h"

namespace Incubator::Scene
{
    std::optional<HierarchyValidationError> findInvalidParentHierarchy(std::span<const EntityDescription> entities)
    {
        for (size_t i = 0; i < entities.size(); ++i)
        {
            const auto& parent = entities[i].parent;
            if (!parent)
                continue;
            if (*parent >= entities.size())
                return HierarchyValidationError{i, "parent index out of range"};
            if (*parent == i)
                return HierarchyValidationError{i, "entity cannot parent itself"};
        }

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
                if (!entities[current].parent)
                    break;
                const size_t next = *entities[current].parent;
                if (visitState[next] == 1)
                    return HierarchyValidationError{i, "parent cycle detected"};
                current = next;
            }
            for (const size_t idx : chain)
                visitState[idx] = 2;
        }
        return std::nullopt;
    }

    bool SceneDescription::valid() const noexcept
    {
        return lighting.valid() && display.valid() &&
               std::ranges::all_of(entities, [](const auto& entity) { return validEntityName(entity.name); }) &&
               std::ranges::all_of(clearColor,
                                   [](float value) { return std::isfinite(value) && value >= 0.0f && value <= 1.0f; }) &&
               !findInvalidParentHierarchy(entities).has_value();
    }

    SceneDescription captureScene(const Scene& scene, std::array<float, 4> clearColor)
    {
        SceneDescription result;
        result.camera = scene.camera();
        result.lighting = scene.lighting();
        result.display = scene.display();
        result.clearColor = clearColor;
        if (!result.valid())
            throw Error::Exception(Error::Code::CONFIG, "Scene background must contain finite values in [0, 1]");
        const auto sceneEntities = scene.entities();
        std::unordered_map<uint64_t, size_t> idToIndex;
        idToIndex.reserve(sceneEntities.size());
        for (size_t i = 0; i < sceneEntities.size(); ++i)
            idToIndex.emplace(sceneEntities[i].id.value(), i);

        result.entities.reserve(sceneEntities.size());
        for (const auto& entity : sceneEntities)
        {
            std::optional<Model::ModelReference> model;
            if (entity.model)
                model = entity.model->source();
            std::optional<size_t> parentIndex;
            if (entity.parent.valid())
                parentIndex = idToIndex.at(entity.parent.value());
            result.entities.push_back({std::move(model), entity.transform, entity.visible, entity.name, parentIndex});
        }
        return result;
    }
}  // namespace Incubator::Scene
