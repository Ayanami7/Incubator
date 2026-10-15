#pragma once

#include <memory>

namespace Incubator
{
    namespace Render
    {
        struct MeshVertex
        {
            float position[3];
            float uv[2];
        };
        static_assert(sizeof(MeshVertex) == 5 * sizeof(float));

        // Copyable reference to one upload, owned until its Renderer is destroyed.
        // Keeping this reference does not extend the lifetime of the Renderer/device.
        class Mesh
        {
        public:
            Mesh() = default;

            bool valid() const noexcept
            {
                return !impl_.expired();
            }

        private:
            friend class Renderer;
            struct Impl;
            explicit Mesh(const std::shared_ptr<Impl>& impl) : impl_(impl) {}
            std::weak_ptr<Impl> impl_;
        };
    }  // namespace Render
}  // namespace Incubator
