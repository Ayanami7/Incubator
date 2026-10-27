#pragma once

#include <array>
#include <optional>
#include <vector>

#include "runtime/model/model_reference.h"
#include "runtime/render/lighting.h"
#include "runtime/scene/camera.h"
#include "runtime/scene/transform.h"

namespace Incubator::Scene
{
    class Scene;

    /// @brief 可独立保存的实体内容，不含场景身份或 GPU 引用。
    struct EntityDescription
    {
        std::optional<Model::ModelReference> model;
        Transform transform;
        bool visible = true;
    };

    /// @brief 独立持有的场景内容及消费者背景设置。
    struct SceneDescription
    {
        std::vector<EntityDescription> entities;
        Camera camera;
        Render::Lighting lighting;
        std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1.0f};

        /// @brief 检查可直接修改的光照及背景值。
        bool valid() const noexcept;
    };

    /// @brief 按创建顺序捕获场景中的所有实体和环境值。
    /// @param scene 待捕获的逻辑场景。
    /// @param clearColor 消费者的 sRGB 背景及线性 alpha，分量均位于 [0, 1]。
    /// @return 独立持有的内容快照，不持有模型或 GPU 资源。
    /// @throws Error::Exception 背景值无效时抛出 CONFIG。
    SceneDescription captureScene(const Scene& scene, std::array<float, 4> clearColor);
}  // namespace Incubator::Scene
