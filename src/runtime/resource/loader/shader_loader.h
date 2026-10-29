#pragma once

#include "runtime/resource/asset_source.h"
#include "runtime/resource/loader/loader_base.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 将已编译的 SPIR-V 资产加载为由 CPU 持有的 ShaderResource 对象。
        class ShaderLoader final : public LoaderBase
        {
        public:
            explicit ShaderLoader(AssetSource& source);

            std::unique_ptr<ResourceObject> load(const ResourceId& id,
                                                 std::string_view virtualPath) override;

        private:
            AssetSource& source_;
        };
    }  // namespace Resource
}  // namespace Incubator
