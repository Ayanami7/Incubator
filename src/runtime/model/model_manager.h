#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "runtime/model/prepared_model.h"

namespace Incubator::Model
{
    /// @brief 同一缓存变体重载前后的模型持有。
    struct ReloadedModel
    {
        std::shared_ptr<const PreparedModel> previous;
        std::shared_ptr<const PreparedModel> current;
    };

    /// @brief 绑定单个 Renderer 会话，按规范化来源及完整选项缓存共享模型。
    /// @note 准备或重载期间借用的 Renderer 必须存活且未移动；不跨会话复用本管理器。
    /// 可以先关闭 Renderer，随后仅查询数量、清理或销毁本管理器。
    class ModelManager final
    {
    public:
        explicit ModelManager(Render::Renderer& renderer) : renderer_(renderer) {}
        ModelManager(const ModelManager&) = delete;
        ModelManager& operator=(const ModelManager&) = delete;

        /// @brief 复用既有模型或同步准备；失败不安装部分模型，原有缓存保持有效。
        std::shared_ptr<const PreparedModel> resolve(const ModelReference& reference);

        /// @brief 从磁盘重新准备指定变体，成功后替换缓存或加入新条目。
        /// @throws LoadError 模型或依赖仍有 CPU 载荷，或重新准备失败。
        std::shared_ptr<const PreparedModel> reload(const ModelReference& reference);

        /// @brief 重新准备路径的所有已缓存变体，全部通过验证后统一替换缓存。
        /// @param virtualPath 将按模型来源路径规则规范化的虚拟路径。
        /// @param validate 提交前同步验证的可选回调；仅在存在匹配变体时调用，不得修改此缓存。
        /// @return 重载前后的模型对；路径未缓存时为空。
        /// @throws LoadError 任一变体重新准备失败；验证回调抛出的异常直接传播，缓存保持原值。
        std::vector<ReloadedModel> reloadByPath(
            std::string_view virtualPath, const std::function<void(std::span<const ReloadedModel>)>& validate = {});

        /// @brief 移除只有缓存持有的模型；场景及外部使用者仍持有的条目保留。
        /// @return 移除的缓存条目数，不等于已销毁 GPU 对象数；在途资源继续由 Fence 保护。
        size_t collectUnused();
        /// @brief 返回缓存条目数，不包含缓存外模型或帧槽单独保留的 GPU 对象。
        size_t size() const noexcept
        {
            return models_.size();
        }

    private:
        std::shared_ptr<const PreparedModel> prepareReload(const ModelReference& reference);

        Render::Renderer& renderer_;
        std::vector<std::shared_ptr<const PreparedModel>> models_;
    };
}  // namespace Incubator::Model
