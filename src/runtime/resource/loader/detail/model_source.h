#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace Incubator::Resource::Detail
{
    // Resolve a source-file-relative dependency into an asset-root-relative key.
    std::string modelDependencyPath(std::string_view directory, std::string_view path, bool uriEncoded);
    std::string decodeModelUri(std::string_view path);

    // Only checks source information that Assimp may discard or accept beyond our support range.
    void checkModelSource(std::string_view path, std::string_view text,
                          std::unordered_set<std::string>& objDiffuseMaterials);
}  // namespace Incubator::Resource::Detail
