#pragma once

#include <array>
#include <memory>

#include "runtime/render/sampler.h"
#include "runtime/render/texture.h"

namespace Incubator
{
    namespace Render
    {
        struct MaterialDesc
        {
            Texture texture;
            // Linear RGB in [0, 1]. Scene output remains opaque.
            std::array<float, 3> color{1.0f, 1.0f, 1.0f};
            SamplerDesc sampler;
        };

        // Immutable material reference. Copies share the same Renderer-owned resource.
        // Keeping this reference does not extend the lifetime of the Renderer/device.
        class Material
        {
        public:
            Material() = default;

            bool valid() const noexcept
            {
                return !impl_.expired();
            }

        private:
            friend class Renderer;
            struct Impl;
            explicit Material(const std::shared_ptr<Impl>& impl) : impl_(impl) {}
            std::weak_ptr<Impl> impl_;
        };
    }  // namespace Render
}  // namespace Incubator
