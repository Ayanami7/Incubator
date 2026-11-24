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
        /// @brief 材质着色模式。
        enum class ShadingMode : uint32_t
        {
            Unlit = 0,
            Lambert = 1,
            Pbr = 2
        };

        /// @brief PBR 标量表面参数（粗糙度 / 金属度）的取值模式。
        enum class MaterialParameterMode : uint32_t
        {
            TextureFactor = 0,  ///< 使用贴图对应通道乘以默认因子（默认导入行为）。
            FixedValue = 1      ///< 忽略贴图对应通道，直接使用固定覆盖值。
        };

        struct MaterialDesc
        {
            Texture texture;
            /// @brief 线性 RGB 颜色，取值范围为 [0, 1]；场景输出保持不透明。
            std::array<float, 3> color{1.0f, 1.0f, 1.0f};
            SamplerDesc sampler;
            ShadingMode shading = ShadingMode::Unlit;
            Texture normalTexture{};             ///< 可选线性法线贴图（切线空间）。
            Texture metallicRoughnessTexture{};  ///< 可选线性金属粗糙度贴图（G=粗糙度，B=金属度）。
            float normalScale = 1.0f;            ///< 法线贴图 XY 缩放系数，有限且 >= 0。
            float metallic = 0.0f;               ///< 金属度因子或固定覆盖值，有限且位于 [0, 1]。
            float roughness = 1.0f;              ///< 粗糙度因子或固定覆盖值，有限且位于 [0, 1]。
            MaterialParameterMode roughnessMode = MaterialParameterMode::TextureFactor;  ///< 粗糙度取值模式。
            MaterialParameterMode metallicMode = MaterialParameterMode::TextureFactor;   ///< 金属度取值模式。
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
