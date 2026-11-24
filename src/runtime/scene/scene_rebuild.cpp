#include "runtime/scene/scene_rebuild.h"

#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/model/prepared_model.h"

namespace Incubator::Scene
{
    RebuiltScene rebuildScene(
        const SceneDescription& description,
        const std::function<std::shared_ptr<const Model::PreparedModel>(const Model::ModelReference&)>& resolve,
        const MaterialOverrideResolver& resolveMaterialOverride)
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
            const auto createdId =
                scene->createEntity(model, entity.transform, entity.visible, entity.name);
            createdIds.push_back(createdId);
            if (!entity.materialOverrides.empty())
            {
                if (!model)
                    throw Error::Exception(Error::Code::CONFIG, "Material override requires an entity model");
                for (const auto& ov : entity.materialOverrides)
                {
                    std::shared_ptr<const Model::PreparedMaterialOverride> compiled;
                    if (resolveMaterialOverride)
                    {
                        compiled = resolveMaterialOverride(*model, ov);
                        if (!compiled)
                            throw Error::Exception(Error::Code::CONFIG, "Material override resolver returned null");
                    }
                    scene->setMaterialOverride(createdId, ov, std::move(compiled));
                }
            }
        }
        for (size_t i = 0; i < description.entities.size(); ++i)
        {
            if (description.entities[i].parent)
                scene->setParent(createdIds[i], createdIds[*description.entities[i].parent]);
        }
        return {std::move(scene), description.clearColor};
    }
}  // namespace Incubator::Scene
