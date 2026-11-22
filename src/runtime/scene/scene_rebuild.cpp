#include "runtime/scene/scene_rebuild.h"

#include <utility>

#include "runtime/core/error/error.h"

namespace Incubator::Scene
{
    RebuiltScene rebuildScene(
        const SceneDescription& description,
        const std::function<std::shared_ptr<const Model::PreparedModel>(const Model::ModelReference&)>& resolve)
    {
        if (const auto hierarchyError = findInvalidParentHierarchy(description.entities))
            throw Error::Exception(
                Error::Code::CONFIG,
                fmt::format("entities[{}].parent: {}", hierarchyError->entityIndex, hierarchyError->reason));
        if (!description.valid())
            throw Error::Exception(Error::Code::CONFIG, "Invalid scene lighting, display, clearColor or entity name");
        for (const auto& entity : description.entities)
        {
            if (!entity.model)
                continue;
            try
            {
                const Model::ModelReference validated(entity.model->path(), entity.model->options());
                if (validated != *entity.model)
                    throw Error::Exception(Error::Code::CONFIG, "Noncanonical model reference");
            }
            catch (const Error::Exception& e)
            {
                throw Error::Exception(Error::Code::CONFIG,
                                       fmt::format("Invalid scene model reference: {}", e.message()));
            }
            if (!resolve)
                throw Error::Exception(Error::Code::CONFIG, "Scene model resolver is missing");
        }
        auto scene = std::make_unique<Scene>();
        scene->setCamera(description.camera);
        scene->setLighting(description.lighting);
        scene->setDisplay(description.display);
        std::vector<EntityId> createdIds;
        createdIds.reserve(description.entities.size());
        for (const auto& entity : description.entities)
        {
            std::shared_ptr<const Model::PreparedModel> model;
            if (entity.model)
            {
                model = resolve(*entity.model);
                if (!model)
                    throw Error::Exception(Error::Code::CONFIG, "Scene model resolver returned null");
            }
            createdIds.push_back(scene->createEntity(std::move(model), entity.transform, entity.visible, entity.name));
        }
        for (size_t i = 0; i < description.entities.size(); ++i)
        {
            if (description.entities[i].parent)
                scene->setParent(createdIds[i], createdIds[*description.entities[i].parent]);
        }
        return {std::move(scene), description.clearColor};
    }
}  // namespace Incubator::Scene
