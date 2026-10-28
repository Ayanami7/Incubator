#include "runtime/scene/scene_frame.h"

#include "runtime/model/model_instance.h"
#include "runtime/scene/frustum.h"
#include "runtime/scene/scene.h"

namespace Incubator::Scene
{
    Render::FrameInput SceneFrame::input() const&
    {
        return {extent, camera, draws, lighting, shadow, shadowDraws};
    }

    std::optional<SceneFrame> extractSceneFrame(const Scene& scene, Render::FrameExtent extent, bool frustumCulling,
                                                const Render::ShadowSettings& shadows)
    {
        if (extent.width == 0 || extent.height == 0)
            return std::nullopt;

        const auto camera = scene.camera().matrices(extent);
        const auto shadow = Render::makeShadowFrame(shadows, scene.lighting().direction);
        const Frustum frustum(camera);
        std::vector<Model::ModelInstance> instances;
        std::vector<Model::ModelInstance> casters;
        instances.reserve(scene.entities().size());
        if (shadow)
            casters.reserve(scene.entities().size());
        for (const auto& entity : scene.entities())
        {
            if (!entity.visible || !entity.model)
                continue;
            const auto transform = entity.transform.matrix();
            if (shadow)
                casters.push_back({entity.model, transform});
            if (frustumCulling && entity.model->bounds())
            {
                const auto worldBounds = entity.model->bounds()->transformed(transform);
                if (worldBounds && !frustum.intersects(*worldBounds))
                    continue;
            }
            instances.push_back({entity.model, transform});
        }
        return SceneFrame{
            extent,           camera, Model::makeModelDraws(instances),
            scene.lighting(), shadow, shadow ? Model::makeModelDraws(casters) : std::vector<Render::DrawItem>{}};
    }
}  // namespace Incubator::Scene
