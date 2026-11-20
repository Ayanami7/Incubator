#pragma once

#include <string>

#include "runtime/resource/resource_id.h"
#include "runtime/resource/resource_type.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 资源定义壳，描述磁盘侧资源的身份与来源。
        ///
        /// 该结构不执行磁盘 I/O，也不持有运行时加载状态。virtualPath 是规范化后的虚拟路径，
        /// 而非机器相关的绝对物理路径。
        struct AssetInfo
        {
            ResourceId id;                              ///< 资源唯一身份。
            ResourceType type = ResourceType::INVALID;  ///< 资源类型标签。
            std::string virtualPath;                    ///< 规范化后的虚拟路径。

            /// @brief 是否具有有效的资源身份。
            ///
            /// 此检查有意保持宽松：不验证 type、virtualPath 及它们与 id 的一致性。
            /// Registry 在登记边界负责维护这些完整性不变量。
            bool valid() const noexcept
            {
                return id.valid();
            }
        };
    }  // namespace Resource
}  // namespace Incubator
