#pragma once

#include <memory>
#include <string_view>

#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/loader/loader_base.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 文本资源加载器
        ///
        /// 把虚拟路径解析为 assets/ 下的真实路径（最小约定，不建 VFS），
        /// 通过注入的 FileIO 读盘并构建 TextResource。
        class TextLoader final : public LoaderBase
        {
        public:
            explicit TextLoader(Incubator::FileIO::FileIO& fileio);

            std::unique_ptr<ResourceObject> load(const ResourceId& id,
                                                 std::string_view virtualPath) override;

        private:
            Incubator::FileIO::FileIO& fileio_;
        };
    }  // namespace Resource
}  // namespace Incubator