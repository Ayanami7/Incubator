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

    std::shared_ptr<const PreparedModel> ModelManager::prepareReload(const ModelReference& reference)
    {
        try
        {
            return prepareModel(renderer_, reference.path(), reference.options(), ModelReadMode::ReloadFromDisk);
        }
        catch (const LoadError&)
        {
            throw;
        }
        catch (const Error::Exception& error)
        {
            throw LoadError(fmt::format("Cannot reload model '{}': {}", reference.path(), error.message()));
        }
    }

    std::shared_ptr<const PreparedModel> ModelManager::reload(const ModelReference& reference)
    {
        auto model = prepareReload(reference);
        const auto found =
            std::ranges::find_if(models_, [&](const auto& cached) { return cached->source() == reference; });
        if (found == models_.end())
            models_.push_back(model);
        else
            *found = model;
        return model;
    }

    std::vector<ReloadedModel> ModelManager::reloadByPath(
        std::string_view virtualPath, const std::function<void(std::span<const ReloadedModel>)>& validate)
    {
        const ModelReference path(virtualPath);
        std::vector<size_t> indices;
        std::vector<ReloadedModel> reloaded;
        for (size_t index = 0; index < models_.size(); ++index)
        {
            const auto& model = models_[index];
            if (model->source().path() != path.path())
                continue;
            reloaded.push_back({model, prepareReload(model->source())});
            indices.push_back(index);
        }
        if (validate && !reloaded.empty())
            validate(reloaded);
        for (size_t index = 0; index < reloaded.size(); ++index)
            models_[indices[index]] = reloaded[index].current;
        return reloaded;
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
