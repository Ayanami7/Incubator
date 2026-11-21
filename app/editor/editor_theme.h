#pragma once

#include <imgui.h>

namespace Incubator::Editor
{
    /// @brief 编辑器现代工作室深色主题语义色板与样式应用入口。
    namespace EditorTheme
    {
        /// @brief 将 sRGB 8 位通道值转换为线性/ImGui RGBA 颜色。
        constexpr ImVec4 rgba(unsigned char r, unsigned char g, unsigned char b, unsigned char a = 255) noexcept
        {
            return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                          static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f);
        }

        /// @brief 错误提示语义色（#F26B63）。
        constexpr ImVec4 errorColor() noexcept
        {
            return rgba(0xF2, 0x6B, 0x63);
        }

        /// @brief 次要/禁用文本语义色（#8C95A4）。
        constexpr ImVec4 mutedTextColor() noexcept
        {
            return rgba(0x8C, 0x95, 0xA4);
        }

        /// @brief 主强调色（#3B78D8，柔和工业蓝）。
        constexpr ImVec4 accentColor() noexcept
        {
            return rgba(0x3B, 0x78, 0xD8);
        }

        /// @brief X 轴标识色（#E64646）。
        constexpr ImVec4 axisXColor() noexcept
        {
            return rgba(0xE6, 0x46, 0x46);
        }

        /// @brief Y 轴标识色（#46D25A）。
        constexpr ImVec4 axisYColor() noexcept
        {
            return rgba(0x46, 0xD2, 0x5A);
        }

        /// @brief Z 轴标识色（#468CF5）。
        constexpr ImVec4 axisZColor() noexcept
        {
            return rgba(0x46, 0x8C, 0xF5);
        }

        /// @brief 资产目录徽章色（琥珀色）。
        constexpr ImVec4 badgeDirColor() noexcept
        {
            return rgba(0xE5, 0xA9, 0x4B);
        }

        /// @brief 资产模型徽章色（蓝色）。
        constexpr ImVec4 badgeModelColor() noexcept
        {
            return rgba(0x5E, 0x9E, 0xF2);
        }

        /// @brief 资产纹理徽章色（绿色）。
        constexpr ImVec4 badgeTextureColor() noexcept
        {
            return rgba(0x5C, 0xC8, 0x7B);
        }

        /// @brief 将现代工作室深色主题应用到当前 ImGui 上下文。
        void applyEditorTheme();
    }  // namespace EditorTheme
}  // namespace Incubator::Editor
