#include "runtime/scene/scene_frame.h"

#include <algorithm>
#include <utility>

#include "runtime/model/prepared_model.h"
#include "runtime/scene/frustum.h"
#include "runtime/scene/scene.h"

namespace Incubator::Scene
{
    Render::FrameInput SceneFrame::input() const&
    {
        return {extent, camera, draws, lighting, shadow, shadowDraws, display, iblEnabled};
    }

    std::optional<SceneFrame> extractSceneFrame(const Scene& scene, Render::FrameExtent extent, bool frustumCulling,
                                                const Render::ShadowSettings& shadows, bool iblEnabled)
    {
        if (extent.width == 0 || extent.height == 0)
            return std::nullopt;

        const auto camera = scene.camera().matrices(extent);
        const auto shadow = Render::makeShadowFrame(shadows, scene.lighting().direction);
        const Frustum frustum(camera);
        std::vector<Render::DrawItem> draws;
        std::vector<Render::DrawItem> shadowDraws;
        const auto appendDraws = [](std::vector<Render::DrawItem>& target, const Model::PreparedModel& model,
                                    const glm::mat4& transform,
                                    std::span<const EntityMaterialOverride> overrides = {}) {
            const auto& modelDraws = model.draws();
            const auto& drawSlots = model.drawMaterialSlots();
            const auto& slots = model.materialSlots();
            for (size_t i = 0; i < modelDraws.size(); ++i)
            {
                auto draw = modelDraws[i];
                draw.model = transform * draw.model;
                if (!overrides.empty() && i < drawSlots.size() && drawSlots[i] < slots.size())
                {
                    const uint32_t sourceSlot = slots[drawSlots[i]].sourceMaterialIndex;
                    const auto found = std::ranges::find(overrides, sourceSlot, [](const EntityMaterialOverride& e) {
                        return e.value.materialSlot;
                    });
                    if (found != overrides.end() && found->compiled && found->compiled->material.valid())
                        draw.material = found->compiled->material;
                }
                target.push_back(std::move(draw));
            }
        };
        for (const auto& entity : scene.entities())
        {
            if (!entity.visible || !entity.model)
                continue;
            const auto world = scene.worldMatrix(entity.id);
            if (!world)
                continue;
            const auto transform = *world;
            if (shadow)
                appendDraws(shadowDraws, *entity.model, transform);
            if (frustumCulling && entity.model->bounds())
            {
                const auto worldBounds = entity.model->bounds()->transformed(transform);
                if (worldBounds && !frustum.intersects(*worldBounds))
                    continue;
            }
            appendDraws(draws, *entity.model, transform, entity.materialOverrides);
        }
        return SceneFrame{extent,           camera,     std::move(draws),
                          scene.lighting(), shadow,     std::move(shadowDraws), scene.display(), iblEnabled};
    }
}  // namespace Incubator::Scene
