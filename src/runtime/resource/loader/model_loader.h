#pragma once

#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/loader/loader_base.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 同步导入静态 glTF 2.0 或 OBJ 模型。
        /// @note FileIO 的生命周期必须长于此 Loader。失败时抛出包含模型和依赖项信息的异常；ResourceManager 记录一次日志并返回 nullptr。
        class ModelLoader final : public LoaderBase
        {
        public:
            explicit ModelLoader(FileIO::FileIO& fileIO);

            /// @brief 加载指定虚拟路径的模型资源。
            /// @param id 已登记的资源标识。
            /// @param virtualPath 模型虚拟路径。
            /// @return 加载得到的 CPU 模型资源。
            std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view virtualPath) override;

        private:
            FileIO::FileIO& fileIO_;
        };
    }  // namespace Resource
}  // namespace Incubator
