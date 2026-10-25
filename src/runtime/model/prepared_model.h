#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "runtime/core/math/aabb.h"
#include "runtime/render/frame_input.h"
#include "runtime/render/sampler.h"
#include "runtime/render/texture.h"
#include "runtime/resource/loader/model_resource.h"

namespace Incubator
{
    namespace Render
    {
        class Renderer;
    }

    namespace Model
    {
        class PreparedModel;

        /// @brief 准备模型时使用的采样与着色预览配置。
        /// @note 源 Unlit 材质始终不受光；不导入源材质的采样器设置。
        struct PrepareOptions
        {
            Render::TextureMipMode mipMode = Render::TextureMipMode::BaseLevel;
            Render::SamplerDesc sampler;
            Render::ShadingMode shading = Render::ShadingMode::Unlit;
        };

        /// @brief 从已注册资源准备可供场景使用的模型。
        /// @param renderer 已初始化且已准备场景着色器的 Renderer。
        /// @param virtualPath 虚拟模型路径。
        /// @param options 模型纹理的 mip、采样及着色配置。
        /// @return 可共享的不可变模型数据。
        /// @throws Error::Exception 加载或准备失败时抛出异常。
        /// @note 需要 MODEL/TEXTURE Loader 路由；仅释放本次调用持有的 CPU 资源。
        std::shared_ptr<const PreparedModel> prepareModel(Render::Renderer& renderer, std::string_view virtualPath,
                                                          PrepareOptions options = {});

        /// @brief 可由模型使用方共享的不可变元数据和 GPU 资源弱引用。
        /// @note 不保留 CPU 资源；GPU 资源归 Renderer 所有，并在 Renderer 关闭时失效。
        class PreparedModel final
        {
        public:
            const std::vector<Render::Mesh>& meshes() const noexcept
            {
                return meshes_;
            }
            const std::vector<Render::Material>& materials() const noexcept
            {
                return materials_;
            }
            const std::vector<Render::Texture>& textures() const noexcept
            {
                return textures_;
            }
            const std::vector<Resource::ModelPart>& parts() const noexcept
            {
                return parts_;
            }
            const std::vector<Resource::ModelNode>& nodes() const noexcept
            {
                return nodes_;
            }
            uint32_t rootNode() const noexcept
            {
                return rootNode_;
            }
            /// @brief 返回按深度优先、父节点先于子节点排列的模型绘制项。
            /// @note 模型矩阵包含源根节点变换。
            const std::vector<Render::DrawItem>& draws() const noexcept
            {
                return draws_;
            }

            /// @brief 返回包含源节点变换的模型整体边界。
            /// @note 不包含实例变换。绘制内容为空或无法计算边界时返回空值；调用方必须保留对应绘制项。
            const std::optional<Math::Aabb>& bounds() const noexcept
            {
                return bounds_;
            }

        private:
            friend std::shared_ptr<const PreparedModel> prepareModel(Render::Renderer&, std::string_view, PrepareOptions);
            PreparedModel() = default;

            std::vector<Render::Mesh> meshes_;
            std::vector<Render::Material> materials_;
            std::vector<Render::Texture> textures_;
            std::vector<Resource::ModelPart> parts_;
            std::vector<Resource::ModelNode> nodes_;
            uint32_t rootNode_ = 0;
            std::vector<Render::DrawItem> draws_;
            std::optional<Math::Aabb> bounds_;
        };
    }  // namespace Model
}  // namespace Incubator
