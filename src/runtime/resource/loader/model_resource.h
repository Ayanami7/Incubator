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
            /// @brief 相对于资产根目录的纹理路径，由导入器解析。
            /// @note 路径为空时使用纯色。
            std::optional<std::string> texturePath;
            std::array<float, 3> color{1.0f, 1.0f, 1.0f};  ///< 线性 RGB；输出不透明。
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

        /// @brief 由 CPU 持有并在构造时验证的模型数据。
        /// @note 所有索引均相对于此资产；节点构成一棵树，rootNode 不必为 0。此处不读取文件，也不创建 GPU 对象。
        class ModelResource final : public ResourceObject
        {
        public:
            ModelResource(ResourceId id, std::vector<MeshData> geometries, std::vector<ModelMaterial> materials,
                          std::vector<ModelPart> parts, std::vector<ModelNode> nodes, uint32_t rootNode);

            const std::vector<MeshData>& geometries() const noexcept
            {
                return geometries_;
            }

            /// @brief 返回各几何体的局部包围盒，与 geometries() 使用相同索引。
            /// @note 包围盒包含未使用的顶点。
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
