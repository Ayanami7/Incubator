#include "runtime/scene/scene_frame.h"

#include <algorithm>
#include <cmath>
#include <iterator>
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
        // 先校验启用的阴影配置与方向，有效时再按投射物边界或级联视锥更新具体矩阵。
        auto shadow = Render::makeShadowFrame(shadows, scene.lighting().direction);
        const Frustum frustum(camera);
        const glm::dvec3 cameraPosition(scene.camera().position());

        struct BlendDraw
        {
            Render::DrawItem item;
            double distanceSq = 0.0;
        };
        std::vector<Render::DrawItem> opaqueDraws;
        std::vector<Render::DrawItem> maskDraws;
        std::vector<BlendDraw> blendDraws;
        std::vector<Render::DrawItem> shadowDraws;
        std::optional<Math::Aabb> casterBounds;

        const auto drawAlphaMode = [](const Model::PreparedModel& model, size_t index) -> Render::AlphaMode {
            const auto& drawSlots = model.drawMaterialSlots();
            const auto& slots = model.materialSlots();
            if (index < drawSlots.size() && drawSlots[index] < slots.size())
                return slots[drawSlots[index]].sourceDesc.alphaMode;
            return Render::AlphaMode::Opaque;
        };

        const auto drawWorldCenter = [](const Model::PreparedModel& model, size_t index, const glm::mat4& transform,
                                        const Render::DrawItem& draw) -> glm::dvec3 {
            const auto& bounds = model.drawBounds();
            if (index < bounds.size() && bounds[index])
            {
                if (const auto worldBounds = bounds[index]->transformed(transform))
                {
                    const glm::dvec3 center = (glm::dvec3(worldBounds->min) + glm::dvec3(worldBounds->max)) * 0.5;
                    if (std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(center.z))
                        return center;
                }
            }
            const glm::dvec3 fallback(draw.model[3]);
            if (std::isfinite(fallback.x) && std::isfinite(fallback.y) && std::isfinite(fallback.z))
                return fallback;
            return glm::dvec3(0.0);
        };

        const auto makeEntityDraw = [](const Model::PreparedModel& model, size_t index, const glm::mat4& transform,
                                       std::span<const EntityMaterialOverride> overrides) -> Render::DrawItem {
            auto draw = model.draws()[index];
            draw.model = transform * draw.model;
            const auto& drawSlots = model.drawMaterialSlots();
            const auto& slots = model.materialSlots();
            if (!overrides.empty() && index < drawSlots.size() && drawSlots[index] < slots.size())
            {
                const uint32_t sourceSlot = slots[drawSlots[index]].sourceMaterialIndex;
                const auto found = std::ranges::find(overrides, sourceSlot, [](const EntityMaterialOverride& e) {
                    return e.value.materialSlot;
                });
                if (found != overrides.end() && found->compiled && found->compiled->material.valid())
                    draw.material = found->compiled->material;
            }
            return draw;
        };

        for (const auto& entity : scene.entities())
        {
            if (!entity.visible || !entity.model)
                continue;
            const auto world = scene.worldMatrix(entity.id);
            if (!world)
                continue;
            const auto transform = *world;
            const auto& model = *entity.model;
            const size_t drawCount = model.draws().size();
            if (shadow)
            {
                bool hasShadowCasterDraw = false;
                for (size_t i = 0; i < drawCount; ++i)
                {
                    if (drawAlphaMode(model, i) == Render::AlphaMode::Blend)
                        continue;
                    shadowDraws.push_back(makeEntityDraw(model, i, transform, {}));
                    hasShadowCasterDraw = true;
                }
                if (hasShadowCasterDraw && model.bounds())
                {
                    if (const auto worldBounds = model.bounds()->transformed(transform))
                    {
                        if (casterBounds)
                            casterBounds->merge(*worldBounds);
                        else
                            casterBounds = worldBounds;
                    }
                }
            }
            if (frustumCulling && model.bounds())
            {
                const auto worldBounds = model.bounds()->transformed(transform);
                if (worldBounds && !frustum.intersects(*worldBounds))
                    continue;
            }
            for (size_t i = 0; i < drawCount; ++i)
            {
                auto draw = makeEntityDraw(model, i, transform, entity.materialOverrides);
                switch (drawAlphaMode(model, i))
                {
                    case Render::AlphaMode::Mask:
                        maskDraws.push_back(std::move(draw));
                        break;
                    case Render::AlphaMode::Blend:
                    {
                        const glm::dvec3 delta = drawWorldCenter(model, i, transform, draw) - cameraPosition;
                        const double distanceSq = glm::dot(delta, delta);
                        blendDraws.push_back({std::move(draw), std::isfinite(distanceSq) ? distanceSq : 0.0});
                        break;
                    }
                    case Render::AlphaMode::Opaque:
                    default:
                        opaqueDraws.push_back(std::move(draw));
                        break;
                }
            }
        }

        if (shadow)
        {
            const auto& perspective = scene.camera().perspective();
            if (shadows.cascadeCount > 1u)
            {
                shadow = Render::makeCascadedShadowFrame(shadows, scene.lighting().direction, camera,
                                                         perspective.nearPlane, perspective.farPlane, casterBounds);
            }
            else if (shadows.fitToBounds && casterBounds)
            {
                shadow = Render::makeShadowFrameForBounds(shadows, scene.lighting().direction, *casterBounds);
            }
        }

        // 半透明绘制项按世界中心到相机的距离从远到近稳定排序，保持不透明与镂空段顺序在前。
        std::stable_sort(blendDraws.begin(), blendDraws.end(), [](const BlendDraw& a, const BlendDraw& b) {
            return a.distanceSq > b.distanceSq;
        });

        std::vector<Render::DrawItem> draws;
        draws.reserve(opaqueDraws.size() + maskDraws.size() + blendDraws.size());
        draws.insert(draws.end(), std::make_move_iterator(opaqueDraws.begin()),
                     std::make_move_iterator(opaqueDraws.end()));
        draws.insert(draws.end(), std::make_move_iterator(maskDraws.begin()), std::make_move_iterator(maskDraws.end()));
        for (auto& blend : blendDraws)
            draws.push_back(std::move(blend.item));

        return SceneFrame{extent,           camera,     std::move(draws),
                          scene.lighting(), shadow,     std::move(shadowDraws), scene.display(), iblEnabled};
    }
}  // namespace Incubator::Scene
