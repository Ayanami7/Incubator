#pragma once

#include <memory>
#include <string_view>

#include "runtime/resource/asset_source.h"
#include "runtime/resource/loader/loader_base.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 文本资源加载器
        ///
        /// 通过借用的 AssetSource 映射虚拟路径，
        /// 通过注入的 FileIO 读盘并构建 TextResource。
        class TextLoader final : public LoaderBase
        {
        public:
            explicit TextLoader(AssetSource& source);

            std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view virtualPath) override;

            LoaderResult loadResult(const ResourceId& id, std::string_view virtualPath) override;

        private:
            AssetSource& source_;
        };
    }  // namespace Resource
}  // namespace Incubator