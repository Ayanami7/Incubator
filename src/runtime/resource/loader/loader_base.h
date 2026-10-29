#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "runtime/resource/resource_id.h"
#include "runtime/resource/resource_object.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 一次 Loader 调用的自有结果；reported 表示旧加载入口已记录失败。
        struct LoaderResult
        {
            std::unique_ptr<ResourceObject> object;
            std::string failure;
            bool reported = false;
        };

        /// @brief 资源加载器抽象
        ///
        /// 负责把资源从磁盘加载并构建为 ResourceObject。
        /// 按 ResourceType 注册到 ResourceManager 的路由表。
        /// @note 失败可返回 nullptr 或透传加载异常；成功对象的 state 由 ResourceManager 置为 READY。
        class LoaderBase
        {
        public:
            virtual ~LoaderBase() = default;

            /// @brief 返回本次调用的具体失败原因；异常仍由 Manager 处理。
            /// @note 默认适配旧 load 入口，其空结果视为已经记录；需传递具体原因时覆盖本入口。
            virtual LoaderResult loadResult(const ResourceId& id, std::string_view path)
            {
                auto object = load(id, path);
                if (!object)
                    return {nullptr, "Loader returned no resource", true};
                return {std::move(object), {}, false};
            }

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
