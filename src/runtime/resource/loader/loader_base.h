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
        /// @note 失败可返回 nullptr 或透传加载异常；成功对象的 state 由 ResourceManager 置为 READY。
        class LoaderBase
        {
        public:
            virtual ~LoaderBase() = default;

            /// @brief 加载并构建 ResourceObject
            /// @param id 资源 ID（Registry 已登记）
            /// @param virtualPath 规范化虚拟路径（AssetInfo 持有）
            /// @return 构建好的对象；已诊断的预期失败可返回 nullptr。
            /// @note 读取/解析异常可以透传。经 ResourceManager 加载时由门面记录并转换为 nullptr；
            ///       直接调用 Loader 的调用方负责处理异常，无需在各 Loader 重复捕获和记录。
            virtual std::unique_ptr<ResourceObject> load(const ResourceId& id,
                                                         std::string_view virtualPath) = 0;
        };
    }  // namespace Resource
}  // namespace Incubator
