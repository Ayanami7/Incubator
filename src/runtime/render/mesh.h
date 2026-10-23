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

        /// @brief 可复制的网格上传资源引用。
        /// @note 资源由 Renderer 持有，直到 Renderer 销毁；持有此引用不会延长 Renderer 或设备的生命周期。
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
