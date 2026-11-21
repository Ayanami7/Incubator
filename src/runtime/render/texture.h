#pragma once

#include <memory>

namespace Incubator
{
    namespace Render
    {
        enum class TextureMipMode
        {
            BaseLevel,
            FullChain
        };

        /// @brief 纹理像素数据的色彩空间，决定上传时使用的 Vulkan 像素格式。
        enum class TextureColorSpace
        {
            Srgb,   ///< VK_FORMAT_R8G8B8A8_SRGB，用于基色/漫反射颜色纹理。
            Linear  ///< VK_FORMAT_R8G8B8A8_UNORM，用于法线、金属粗糙度等线性数据纹理。
        };

        /// @brief 可复制的同步纹理上传资源引用。
        /// @note 非拥有引用；实际对象由 Renderer、GpuResources、材质依赖或在途帧持有，不延长设备生命周期。
        class Texture
        {
        public:
            Texture() = default;

            bool valid() const noexcept
            {
                return !impl_.expired();
            }

        private:
            friend class Renderer;
            friend class Material;
            struct Impl;
            explicit Texture(const std::shared_ptr<Impl>& impl) : impl_(impl) {}
            bool isUnset() const noexcept
            {
                return !impl_.owner_before(std::weak_ptr<Impl>{}) && !std::weak_ptr<Impl>{}.owner_before(impl_);
            }
            std::weak_ptr<Impl> impl_;
        };
    }  // namespace Render
}  // namespace Incubator
