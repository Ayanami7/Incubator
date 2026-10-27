#include "runtime/scene/scene_description.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/model/prepared_model.h"
#include "runtime/scene/scene.h"

namespace Incubator::Scene
{
    bool SceneDescription::valid() const noexcept
    {
        return lighting.valid() && std::ranges::all_of(clearColor, [](float value) {
                   return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
               });
    }

    SceneDescription captureScene(const Scene& scene, std::array<float, 4> clearColor)
    {
        SceneDescription result;
        result.camera = scene.camera();
        result.lighting = scene.lighting();
        result.clearColor = clearColor;
        if (!result.valid())
            throw Error::Exception(Error::Code::CONFIG, "Scene background must contain finite values in [0, 1]");
        result.entities.reserve(scene.entities().size());
        for (const auto& entity : scene.entities())
        {
            std::optional<Model::ModelReference> model;
            if (entity.model)
                model = entity.model->source();
            result.entities.push_back({std::move(model), entity.transform, entity.visible});
        }
        return result;
    }
}  // namespace Incubator::Scene
