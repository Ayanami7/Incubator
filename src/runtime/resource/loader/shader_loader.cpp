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
        ShaderLoader::ShaderLoader(AssetSource& source) : source_(source) {}

        std::unique_ptr<ResourceObject> ShaderLoader::load(const ResourceId& id, std::string_view virtualPath)
        {
            const auto assetPath = source_.resolve(virtualPath);
            auto words = Loader::readSpirv(source_.fileIO(), assetPath);
            return std::make_unique<ShaderResource>(id, std::move(words));
        }
    }  // namespace Resource
}  // namespace Incubator
