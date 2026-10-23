#pragma once

#include <cstdint>
#include <memory>

#include "runtime/scene/transform.h"

namespace Incubator::Model
{
    class PreparedModel;
}

namespace Incubator::Scene
{
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
    };
}  // namespace Incubator::Scene
