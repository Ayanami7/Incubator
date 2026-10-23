#pragma once

#include <array>
#include <cstdint>
#include <glm/mat4x4.hpp>
#include <optional>
#include <string>
#include <vector>

#include "runtime/core/math/aabb.h"
#include "runtime/resource/mesh_data.h"
#include "runtime/resource/resource_object.h"

namespace Incubator
{
    namespace Resource
    {
        struct ModelMaterial
        {
            // Asset-root-relative path, resolved by the importer. No path means solid color.
            std::optional<std::string> texturePath;
            std::array<float, 3> color{1.0f, 1.0f, 1.0f};  // Linear RGB; opaque output.
        };

        struct ModelPart
        {
            uint32_t geometry = 0;
            uint32_t material = 0;
        };

        struct ModelNode
        {
            std::string name;
            glm::mat4 localTransform{1.0f};
            std::vector<uint32_t> children;
            std::vector<uint32_t> parts;
        };

        // Owned CPU model, validated once at construction. All indices are local to this asset.
        // Nodes form one tree; rootNode need not be zero. No file reads or GPU objects are created.
        class ModelResource final : public ResourceObject
        {
        public:
            ModelResource(ResourceId id, std::vector<MeshData> geometries, std::vector<ModelMaterial> materials,
                          std::vector<ModelPart> parts, std::vector<ModelNode> nodes, uint32_t rootNode);

            const std::vector<MeshData>& geometries() const noexcept
            {
                return geometries_;
            }

            // Geometry-local bounds, indexed exactly like geometries(). Includes unused vertices.
            const std::vector<Math::Aabb>& geometryBounds() const noexcept
            {
                return geometryBounds_;
            }

            const std::vector<ModelMaterial>& materials() const noexcept
            {
                return materials_;
            }

            const std::vector<ModelPart>& parts() const noexcept
            {
                return parts_;
            }

            const std::vector<ModelNode>& nodes() const noexcept
            {
                return nodes_;
            }

            uint32_t rootNode() const noexcept
            {
                return rootNode_;
            }

        private:
            std::vector<MeshData> geometries_;
            std::vector<Math::Aabb> geometryBounds_;
            std::vector<ModelMaterial> materials_;
            std::vector<ModelPart> parts_;
            std::vector<ModelNode> nodes_;
            uint32_t rootNode_;
        };
    }  // namespace Resource
}  // namespace Incubator
