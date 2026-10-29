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
        TextLoader::TextLoader(AssetSource& source) : source_(source) {}

        std::unique_ptr<ResourceObject> TextLoader::load(const ResourceId& id, std::string_view virtualPath)
        {
            auto result = loadResult(id, virtualPath);
            if (!result.object)
                Log::Registry::get("resource").error("{}", result.failure);
            return std::move(result.object);
        }

        LoaderResult TextLoader::loadResult(const ResourceId& id, std::string_view virtualPath)
        {
            const auto realPath = source_.resolve(virtualPath);
            const auto realPathUtf8 = Utils::Utf8::toUtf8String(realPath);
            const uint64_t size = source_.fileIO().getFileSize(realPath);
            auto handle = source_.open(virtualPath, FileIO::ShareMode::Exclusive);
            if (!handle)
                return {nullptr, fmt::format("TextLoader: cannot open '{}'", realPathUtf8), false};
            std::string text(static_cast<size_t>(size), '\0');
            const size_t read = handle->read(std::span<uint8_t>(reinterpret_cast<uint8_t*>(text.data()), text.size()));
            text.resize(read);
            return {std::make_unique<TextResource>(id, std::move(text)), {}, false};
        }
    }  // namespace Resource
}  // namespace Incubator
