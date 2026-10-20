#pragma once

#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/loader/loader_base.h"

namespace Incubator
{
    namespace Resource
    {
        // Synchronous static glTF 2.0 / OBJ import. FileIO must outlive this Loader.
        // Throws with model/dependency context; ResourceManager logs once and returns nullptr.
        class ModelLoader final : public LoaderBase
        {
        public:
            explicit ModelLoader(FileIO::FileIO& fileIO);

            std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view virtualPath) override;

        private:
            FileIO::FileIO& fileIO_;
        };
    }  // namespace Resource
}  // namespace Incubator
