#pragma once

#include <memory>
#include <span>
#include <vector>

#include "runtime/model/prepared_model.h"

namespace Incubator::Model
{
    struct ModelInstance
    {
        std::shared_ptr<const PreparedModel> model;
        glm::mat4 transform{1.0f};
    };

    // Returns a fresh list: instance order, then each model's depth-first node/part order.
    // Each matrix is instance.transform * cached model-relative matrix. Null models throw CONFIG;
    // GPU reference validity is checked by Renderer at submission, not during expansion.
    std::vector<Render::DrawItem> makeModelDraws(std::span<const ModelInstance> instances);
}  // namespace Incubator::Model
