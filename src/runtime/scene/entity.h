#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "runtime/core/utils/utf8.h"
#include "runtime/scene/transform.h"

namespace Incubator::Model
{
    class PreparedModel;
}

namespace Incubator::Scene
{
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
    };
}  // namespace Incubator::Scene
