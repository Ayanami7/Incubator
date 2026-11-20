#include "runtime/scene/scene_frame.h"

#include <utility>

#include "runtime/model/prepared_model.h"
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
        std::vector<Render::DrawItem> draws;
        std::vector<Render::DrawItem> shadowDraws;
        const auto appendDraws = [](std::vector<Render::DrawItem>& target, const Model::PreparedModel& model,
                                    const glm::mat4& transform) {
            for (auto draw : model.draws())
            {
                draw.model = transform * draw.model;
                target.push_back(std::move(draw));
            }
        };
        for (const auto& entity : scene.entities())
        {
            if (!entity.visible || !entity.model)
                continue;
            const auto transform = entity.transform.matrix();
            if (shadow)
                appendDraws(shadowDraws, *entity.model, transform);
            if (frustumCulling && entity.model->bounds())
            {
                const auto worldBounds = entity.model->bounds()->transformed(transform);
                if (worldBounds && !frustum.intersects(*worldBounds))
                    continue;
            }
            appendDraws(draws, *entity.model, transform);
        }
        return SceneFrame{extent,           camera, std::move(draws),
                          scene.lighting(), shadow, std::move(shadowDraws)};
    }
}  // namespace Incubator::Scene
