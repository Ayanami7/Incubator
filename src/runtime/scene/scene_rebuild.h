#pragma once

#include <array>
#include <functional>
#include <memory>

#include "runtime/scene/scene.h"
#include "runtime/scene/scene_description.h"

namespace Incubator::Scene
{
    /// @brief 新场景与由消费者持有的背景设置。
    struct RebuiltScene
    {
        std::unique_ptr<Scene> scene;
        std::array<float, 4> clearColor;
    };

    /// @brief 使用调用方解析器重建完整场景；成功后由调用方发布。
    /// @param description 独立场景内容。
    /// @param resolve 按来源获取本会话的共享模型。
    /// @return 新场景与背景；实体使用新分配的 ID。
    /// @throws Error::Exception 内容无效或解析器返回空模型时抛 CONFIG；解析异常直接传播。
    RebuiltScene rebuildScene(
        const SceneDescription& description,
        const std::function<std::shared_ptr<const Model::PreparedModel>(const Model::ModelReference&)>& resolve);
}  // namespace Incubator::Scene
