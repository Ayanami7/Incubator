#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/core/utils/utf8.h"
#include "runtime/render/material.h"
#include "runtime/scene/transform.h"

namespace Incubator::Model
{
    class PreparedModel;
    struct PreparedMaterialOverride;
}

namespace Incubator::Scene
{
    /// @brief 实体对源模型指定材质槽的显式 PBR 参数覆盖值（纯数据，不含 GPU 引用）。
    struct MaterialOverride
    {
        uint32_t materialSlot = 0;
        std::array<float, 3> color{1.0f, 1.0f, 1.0f};
        Render::MaterialParameterMode roughnessMode = Render::MaterialParameterMode::TextureFactor;
        float roughness = 1.0f;
        Render::MaterialParameterMode metallicMode = Render::MaterialParameterMode::TextureFactor;
        float metallic = 0.0f;

        bool operator==(const MaterialOverride&) const = default;

        bool valid() const noexcept
        {
            for (const float c : color)
                if (!std::isfinite(c) || c < 0.0f || c > 1.0f)
                    return false;
            if ((roughnessMode != Render::MaterialParameterMode::TextureFactor &&
                 roughnessMode != Render::MaterialParameterMode::FixedValue) ||
                (metallicMode != Render::MaterialParameterMode::TextureFactor &&
                 metallicMode != Render::MaterialParameterMode::FixedValue))
                return false;
            return std::isfinite(roughness) && roughness >= 0.0f && roughness <= 1.0f && std::isfinite(metallic) &&
                   metallic >= 0.0f && metallic <= 1.0f;
        }
    };

    /// @brief 实体持有的材质槽覆盖条目，同时关联可按需替换的不可变 GPU 覆盖材质引用。
    struct EntityMaterialOverride
    {
        MaterialOverride value;
        std::shared_ptr<const Model::PreparedMaterialOverride> compiled;
    };
    /// @brief 检查实体名称是否为不含 NUL 的 UTF-8；空名称合法。
    inline bool validEntityName(std::string_view name)
    {
        return name.find('\0') == std::string_view::npos && Utils::Utf8::isValid(name);
    }
    class Scene;

    /// @brief 进程内的逻辑实体标识，与资源及 GPU/模型索引无关。
    /// @note 非零表示标识已发放；只有所属 Scene 能判断实体是否仍然有效。
    class EntityId
    {
    public:
        EntityId() = default;

        bool valid() const noexcept
        {
            return value_ != 0;
        }

        uint64_t value() const noexcept
        {
            return value_;
        }

        bool operator==(const EntityId&) const = default;

    private:
        friend class Scene;
        explicit EntityId(uint64_t value) : value_(value) {}
        uint64_t value_ = 0;
    };

    /// @brief Scene 中的实体数据。
    /// @note Scene 仅公开只读借用的实体；通过基于 ID 的接口修改已存储的值。
    struct Entity
    {
        EntityId id;
        Transform transform;
        bool visible = true;
        std::shared_ptr<const Model::PreparedModel> model;
        std::string name;
        EntityId parent;
        std::vector<EntityMaterialOverride> materialOverrides;
    };
}  // namespace Incubator::Scene
