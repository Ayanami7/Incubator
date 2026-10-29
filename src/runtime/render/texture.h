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
            std::weak_ptr<Impl> impl_;
        };
    }  // namespace Render
}  // namespace Incubator
