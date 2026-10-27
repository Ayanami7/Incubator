#pragma once

#include <string>
#include <string_view>

#include "runtime/render/material.h"

namespace Incubator::Model
{
    /// @brief 准备模型时使用的采样与着色预览配置。
    /// @note 源 Unlit 材质始终不受光；不导入源材质的采样器设置。
    struct PrepareOptions
    {
        Render::TextureMipMode mipMode = Render::TextureMipMode::BaseLevel;
        Render::SamplerDesc sampler;
        Render::ShadingMode shading = Render::ShadingMode::Unlit;
        bool operator==(const PrepareOptions&) const = default;
    };

    /// @brief 拥有规范化虚拟路径和完整准备配置的模型来源值。
    class ModelReference final
    {
    public:
        /// @param virtualPath 相对资产根的 UTF-8 虚拟路径。
        /// @param options 请求的模型准备配置。
        /// @throws Error::Exception 路径或配置无效时抛出 RENDER。
        explicit ModelReference(std::string_view virtualPath, PrepareOptions options = {});

        /// @brief 返回规范化后的虚拟资源路径。
        const std::string& path() const noexcept
        {
            return path_;
        }
        /// @brief 返回原样保存的准备配置。
        const PrepareOptions& options() const noexcept
        {
            return options_;
        }
        bool operator==(const ModelReference&) const = default;

    private:
        std::string path_;
        PrepareOptions options_;
    };
}  // namespace Incubator::Model
