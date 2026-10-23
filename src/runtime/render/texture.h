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
        /// @note 资源由 Renderer 持有，直到 Renderer 销毁；持有此引用不会延长 Renderer 或设备的生命周期。
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
