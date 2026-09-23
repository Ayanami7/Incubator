#pragma once

#include <memory>
#include <string_view>

#include "runtime/resource/resource_id.h"
#include "runtime/resource/resource_object.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 资源加载器抽象
        ///
        /// 负责把资源从磁盘加载并构建为 ResourceObject。
        /// 按 ResourceType 注册到 ResourceManager 的路由表。
        /// @note 失败返回 nullptr（不抛异常）；成功返回的对象 state 由 ResourceManager 置为 READY。
        class LoaderBase
        {
        public:
            virtual ~LoaderBase() = default;

            /// @brief 加载并构建 ResourceObject
            /// @param id 资源 ID（Registry 已登记）
            /// @param virtualPath 规范化虚拟路径（AssetInfo 持有）
            /// @return 构建好的对象；失败返回 nullptr
            /// @note 不允许向上抛异常：无论底层 IO 抛什么，都应在此转换为 nullptr（Manager 门面会兜底捕获）
            virtual std::unique_ptr<ResourceObject> load(const ResourceId& id,
                                                         std::string_view virtualPath) = 0;
        };
    }  // namespace Resource
}  // namespace Incubator