#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/math/aabb.h"
#include "runtime/model/model_reference.h"
#include "runtime/render/frame_input.h"
#include "runtime/render/gpu_resources.h"
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

        /// @brief 资源门面已经记录的 CPU 模型或依赖加载失败，异常保留模型上下文与原始原因。
        /// @note 消费者可以展示该异常，但不再次记录同一失败；GPU 创建异常仍沿用 Renderer 契约。
        class LoadError final : public Error::Exception
        {
        public:
            explicit LoadError(std::string message) : Error::Exception(Error::Code::RENDER, std::move(message)) {}
        };

        /// @brief 通过资源门面准备可供场景使用的模型。
        /// @param renderer 已初始化且已准备场景着色器的 Renderer。
        /// @param virtualPath 虚拟模型路径。
        /// @param options 模型纹理的 mip、采样及着色配置。
        /// @return 可共享的不可变模型数据。
        /// @throws Error::Exception 加载或准备失败时抛出异常。
        /// @note 需要 MODEL/TEXTURE Loader 路由；成功释放本次 CPU 持有，失败同时释放本次未发布 GPU 资源。
        std::shared_ptr<const PreparedModel> prepareModel(Render::Renderer& renderer, std::string_view virtualPath,
                                                          PrepareOptions options = {});

        /// @brief 通过 shared_ptr 共享的不可变元数据与 GPU 资源组。
        /// @note 不保留 CPU 资源；最后模型持有结束后撤销本组 GPU 持有，在途帧另受 Fence 保护。
        /// 提取至提交期间须保留模型；Renderer 先关闭时 GPU 引用失效，模型晚析构安全。
        class PreparedModel final
        {
        public:
            /// @brief 返回准备时请求的来源路径与配置。
            const ModelReference& source() const noexcept
            {
                return source_;
            }
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
            explicit PreparedModel(ModelReference source) : source_(std::move(source)) {}

            ModelReference source_;
            Render::GpuResources resources_;
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
