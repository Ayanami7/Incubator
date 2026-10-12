#include "runtime/resource/loader/shader_resource.h"

#include <utility>

#include "runtime/resource/resource_type.h"

namespace Incubator
{
    namespace Resource
    {
        ShaderResource::ShaderResource(ResourceId id, std::vector<uint32_t> words)
            : ResourceObject(id, ResourceType::SHADER), words_(std::move(words))
        {
        }
    }  // namespace Resource
}  // namespace Incubator
