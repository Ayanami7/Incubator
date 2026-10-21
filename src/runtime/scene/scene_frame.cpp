#include "runtime/scene/scene_frame.h"

#include "runtime/model/model_instance.h"
#include "runtime/scene/scene.h"

namespace Incubator::Scene
{
    Render::FrameInput SceneFrame::input() const&
    {
        return {extent, camera, draws};
    }

    std::optional<SceneFrame> extractSceneFrame(const Scene& scene, Render::FrameExtent extent)
    {
        if (extent.width == 0 || extent.height == 0)
            return std::nullopt;

        std::vector<Model::ModelInstance> instances;
        instances.reserve(scene.entities().size());
        for (const auto& entity : scene.entities())
            if (entity.visible && entity.model)
                instances.push_back({entity.model, entity.transform.matrix()});
        return SceneFrame{extent, scene.camera().matrices(extent), Model::makeModelDraws(instances)};
    }
}  // namespace Incubator::Scene
