#include "runtime/resource/loader/text_loader.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "runtime/core/log/logger.h"
#include "runtime/core/path/path.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/resource/loader/text_resource.h"

namespace Incubator
{
    namespace Resource
    {
        TextLoader::TextLoader(Incubator::FileIO::FileIO& fileio) : fileio_(fileio) {}

        std::unique_ptr<ResourceObject> TextLoader::load(const ResourceId& id,
                                                         std::string_view virtualPath)
        {
            const std::string realPathUtf8 = Incubator::Path::join("assets", virtualPath);
            const auto realPath = Incubator::Utils::Utf8::toFsPath(realPathUtf8);

            uint64_t size = fileio_.getFileSize(realPath);
            auto handle = fileio_.open(realPath, Incubator::FileIO::OpenMode::Read);
            if (!handle)
            {
                Log::Registry::get("resource").error("TextLoader: cannot open '{}'", realPathUtf8);
                return nullptr;
            }

            std::string text(static_cast<size_t>(size), '\0');
            size_t read = handle->read(
                std::span<uint8_t>(reinterpret_cast<uint8_t*>(text.data()), text.size()));
            text.resize(read);

            return std::make_unique<TextResource>(id, std::move(text));
        }
    }  // namespace Resource
}  // namespace Incubator
