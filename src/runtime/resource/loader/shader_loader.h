#pragma once

#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/loader/loader_base.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief Loads compiled SPIR-V assets into CPU-owned ShaderResource objects.
        class ShaderLoader final : public LoaderBase
        {
        public:
            explicit ShaderLoader(FileIO::FileIO& fileIO);

            std::unique_ptr<ResourceObject> load(const ResourceId& id,
                                                 std::string_view virtualPath) override;

        private:
            FileIO::FileIO& fileIO_;
        };
    }  // namespace Resource
}  // namespace Incubator
