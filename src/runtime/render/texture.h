#pragma once

#include <memory>

namespace Incubator
{
    namespace Render
    {
        // Copyable reference to one synchronous upload, owned until its Renderer is destroyed.
        // Keeping this reference does not extend the lifetime of the Renderer/device.
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
