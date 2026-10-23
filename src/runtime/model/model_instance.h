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

    /// @brief 将模型实例展开为按实例及节点顺序排列的绘制项。
    /// @param instances 待展开的模型实例。
    /// @return 新的绘制项列表；矩阵为 instance.transform * 缓存的模型相对矩阵。
    /// @throws Error::Exception 任一模型为空时抛出 CONFIG 错误。
    /// @note GPU 引用有效性由 Renderer 在提交时检查，不在展开列表时检查。
    std::vector<Render::DrawItem> makeModelDraws(std::span<const ModelInstance> instances);
}  // namespace Incubator::Model
