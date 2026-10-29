#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "runtime/render/sampler.h"
#include "runtime/render/texture.h"

namespace Incubator
{
    namespace Render
    {
        /// @brief 最小材质着色模式；Lambert 为方向光漫反射预览。
        enum class ShadingMode : uint32_t
        {
            Unlit = 0,
            Lambert = 1
        };

        struct MaterialDesc
        {
            Texture texture;
            /// @brief 线性 RGB 颜色，取值范围为 [0, 1]；场景输出保持不透明。
            std::array<float, 3> color{1.0f, 1.0f, 1.0f};
            SamplerDesc sampler;
            ShadingMode shading = ShadingMode::Unlit;
        };

        /// @brief 不可变材质的非拥有引用，副本指向同一份实际对象。
        /// @note 持有此引用不会延长 Renderer 或设备的生命周期。
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
