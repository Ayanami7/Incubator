#include "runtime/model/model_manager.h"

#include <algorithm>

#include "runtime/core/log/logger.h"

namespace Incubator::Model
{
    std::shared_ptr<const PreparedModel> ModelManager::resolve(const ModelReference& reference)
    {
        const auto found =
            std::ranges::find_if(models_, [&](const auto& model) { return model->source() == reference; });
        if (found != models_.end())
            return *found;
        auto model = prepareModel(renderer_, reference.path(), reference.options());
        models_.push_back(model);
        return model;
    }

    size_t ModelManager::collectUnused()
    {
        return std::erase_if(models_, [](const auto& model) {
            if (model.use_count() != 1)
                return false;
            Log::Registry::get("model").debug("Removed unused cached model '{}': {} meshes, {} materials, {} textures",
                                              model->source().path(), model->meshes().size(), model->materials().size(),
                                              model->textures().size());
            return true;
        });
    }
}  // namespace Incubator::Model
