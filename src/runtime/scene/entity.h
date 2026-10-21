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

    // Process-local logical identity, independent of resources and GPU/model indices.
    // Nonzero means issued; only the owning Scene can determine whether it is still alive.
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

    // Scene exposes const borrowed entities; mutate stored values through its ID-based APIs.
    struct Entity
    {
        EntityId id;
        Transform transform;
        bool visible = true;
        std::shared_ptr<const Model::PreparedModel> model;
    };
}  // namespace Incubator::Scene
