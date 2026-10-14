#include "runtime/resource/loader/shader_loader.h"

#include <string>
#include <utility>

#include "runtime/core/path/path.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/loader/spirv_reader.h"

namespace Incubator
{
    namespace Resource
    {
        ShaderLoader::ShaderLoader(FileIO::FileIO& fileIO) : fileIO_(fileIO) {}

        std::unique_ptr<ResourceObject> ShaderLoader::load(const ResourceId& id,
                                                           std::string_view virtualPath)
        {
            const std::string assetPathUtf8 = Path::join("assets", virtualPath);
            const auto assetPath = Utils::Utf8::toFsPath(assetPathUtf8);
            auto words = Loader::readSpirv(fileIO_, assetPath);
            return std::make_unique<ShaderResource>(id, std::move(words));
        }
    }  // namespace Resource
}  // namespace Incubator
