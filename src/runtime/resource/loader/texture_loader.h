#pragma once

#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/loader/loader_base.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief Loads non-interlaced 8-bit Gray/RGB/Gray+Alpha/RGBA PNG into owned RGBA8 pixels.
        /// Reads through FileIO and throws on failure; ResourceManager logs once and returns nullptr.
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
