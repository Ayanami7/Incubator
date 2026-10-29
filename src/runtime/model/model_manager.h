#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "runtime/model/prepared_model.h"

namespace Incubator::Model
{
    /// @brief 绑定单个 Renderer 会话，按规范化来源及完整选项缓存共享模型。
    /// @note resolve 期间借用的 Renderer 必须存活且未移动；不跨会话复用本管理器。
    /// 可以先关闭 Renderer，随后仅查询数量、清理或销毁本管理器，不再调用 resolve。
    class ModelManager final
    {
    public:
        explicit ModelManager(Render::Renderer& renderer) : renderer_(renderer) {}
        ModelManager(const ModelManager&) = delete;
        ModelManager& operator=(const ModelManager&) = delete;

        /// @brief 复用既有模型或同步准备；失败不安装部分模型，原有缓存保持有效。
        std::shared_ptr<const PreparedModel> resolve(const ModelReference& reference);
        /// @brief 移除只有缓存持有的模型；场景及外部使用者仍持有的条目保留。
        /// @return 移除的缓存条目数，不等于已销毁 GPU 对象数；在途资源继续由 Fence 保护。
        size_t collectUnused();
        /// @brief 返回缓存条目数，不包含缓存外模型或帧槽单独保留的 GPU 对象。
        size_t size() const noexcept
        {
            return models_.size();
        }

    private:
        Render::Renderer& renderer_;
        std::vector<std::shared_ptr<const PreparedModel>> models_;
    };
}  // namespace Incubator::Model
