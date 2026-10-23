#pragma once

#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/loader/loader_base.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 将非隔行、8 位灰度/RGB/灰度加 Alpha/RGBA PNG 加载为由 CPU 持有的 RGBA8 像素。
        /// 通过 FileIO 读取；失败时抛出异常，ResourceManager 记录一次日志并返回 nullptr。
        class TextureLoader final : public LoaderBase
        {
        public:
            explicit TextureLoader(FileIO::FileIO& fileIO);

            std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view virtualPath) override;

        private:
            FileIO::FileIO& fileIO_;
        };
    }  // namespace Resource
}  // namespace Incubator
