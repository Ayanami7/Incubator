#include "runtime/resource/loader/text_resource.h"

#include <utility>

#include "runtime/resource/resource_type.h"

namespace Incubator
{
    namespace Resource
    {
        TextResource::TextResource(ResourceId id, std::string text)
            : ResourceObject(id, ResourceType::TEXT), text_(std::move(text))
        {
        }
    }  // namespace Resource
}  // namespace Incubator