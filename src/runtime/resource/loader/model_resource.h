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
        /// @brief 源材质的透明度模式。
        enum class AlphaMode : uint32_t
        {
            Opaque,
            Mask,
            Blend
        };

        struct ModelMaterial
        {
            /// @brief 相对于资产根目录的纹理路径，由导入器解析。
            /// @note 基色贴图可使用外部路径或内嵌索引。两个字段均为空时使用纯色。
            std::optional<std::string> texturePath;
            std::array<float, 3> color{1.0f, 1.0f, 1.0f};  ///< 线性 RGB；输出不透明。
            bool unlit = false;  ///< 源材质显式声明不受光；不包含消费者的着色策略。
            std::optional<std::string> normalTexturePath;             ///< 相对资产根的线性法线贴图路径。
            std::optional<std::string> metallicRoughnessTexturePath;  ///< 线性金属粗糙度贴图路径，G 为粗糙度，B 为金属度。
            float normalScale = 1.0f;                                 ///< 法线贴图 XY 缩放系数，有限且 >= 0。
            float metallic = 0.0f;                                    ///< 金属度因子，有限且位于 [0, 1]。
            float roughness = 1.0f;                                   ///< 粗糙度因子，有限且位于 [0, 1]。
            std::optional<uint32_t> embeddedTextureIndex;                 ///< 内嵌基色贴图索引。
            std::optional<uint32_t> embeddedNormalTextureIndex;            ///< 内嵌法线贴图索引。
            std::optional<uint32_t> embeddedMetallicRoughnessTextureIndex; ///< 内嵌金属粗糙度贴图索引。
            AlphaMode alphaMode = AlphaMode::Opaque;
            float alphaCutoff = 0.5f;
            float baseAlpha = 1.0f;
            bool doubleSided = false;
            std::array<float, 3> emissiveFactor{};
            std::optional<std::string> emissiveTexturePath;      ///< 相对资产根的自发光贴图路径。
            std::optional<uint32_t> embeddedEmissiveTextureIndex; ///< 内嵌自发光贴图索引。
        };

        /// @brief 模型拥有的紧密 RGBA8 像素，颜色空间由使用该贴图的材质槽决定。
        struct EmbeddedTexture
        {
            uint32_t width;
            uint32_t height;
            std::vector<uint8_t> rgba8;
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
                          std::vector<ModelPart> parts, std::vector<ModelNode> nodes, uint32_t rootNode,
                          std::vector<EmbeddedTexture> embeddedTextures = {});

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

            /// @brief 返回材质的内嵌索引所引用的图片数据。
            const std::vector<EmbeddedTexture>& embeddedTextures() const noexcept
            {
                return embeddedTextures_;
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
            std::vector<EmbeddedTexture> embeddedTextures_;
            std::vector<ModelPart> parts_;
            std::vector<ModelNode> nodes_;
            uint32_t rootNode_;
        };
    }  // namespace Resource
}  // namespace Incubator
