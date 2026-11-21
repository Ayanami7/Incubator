#include "editor_theme.h"

namespace Incubator::Editor::EditorTheme
{
    void applyEditorTheme()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::StyleColorsDark(&style);

        // 统一边框、圆角与间距度量
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 1.0f;
        style.TabBorderSize = 0.0f;

        style.WindowRounding = 4.0f;
        style.ChildRounding = 3.0f;
        style.FrameRounding = 3.0f;
        style.PopupRounding = 4.0f;
        style.ScrollbarRounding = 4.0f;
        style.GrabRounding = 3.0f;
        style.TabRounding = 3.0f;

        style.WindowPadding = ImVec2(10.0f, 10.0f);
        style.FramePadding = ImVec2(8.0f, 5.0f);
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
        style.IndentSpacing = 16.0f;
        style.ScrollbarSize = 13.0f;
        style.GrabMinSize = 10.0f;

        ImVec4* colors = style.Colors;

        // 文本层级
        colors[ImGuiCol_Text] = rgba(0xE6, 0xEA, 0xF0);
        colors[ImGuiCol_TextDisabled] = mutedTextColor();

        // 背景层级
        colors[ImGuiCol_WindowBg] = rgba(0x1E, 0x21, 0x26);
        colors[ImGuiCol_ChildBg] = rgba(0x18, 0x1B, 0x20);
        colors[ImGuiCol_PopupBg] = rgba(0x22, 0x26, 0x2C);
        colors[ImGuiCol_MenuBarBg] = rgba(0x19, 0x1C, 0x20);
        colors[ImGuiCol_DockingEmptyBg] = rgba(0x16, 0x18, 0x1B);

        // 边框与分隔线
        colors[ImGuiCol_Border] = rgba(0x32, 0x37, 0x41);
        colors[ImGuiCol_BorderShadow] = rgba(0x00, 0x00, 0x00, 0x00);
        colors[ImGuiCol_Separator] = rgba(0x30, 0x35, 0x3F);
        colors[ImGuiCol_SeparatorHovered] = rgba(0x45, 0x4E, 0x5E);
        colors[ImGuiCol_SeparatorActive] = accentColor();

        // 标题栏与折叠分组
        colors[ImGuiCol_TitleBg] = rgba(0x16, 0x18, 0x1B);
        colors[ImGuiCol_TitleBgActive] = rgba(0x1E, 0x22, 0x28);
        colors[ImGuiCol_TitleBgCollapsed] = rgba(0x16, 0x18, 0x1B);

        colors[ImGuiCol_Header] = rgba(0x28, 0x2D, 0x35);
        colors[ImGuiCol_HeaderHovered] = rgba(0x32, 0x38, 0x42);
        colors[ImGuiCol_HeaderActive] = rgba(0x3A, 0x42, 0x4E);

        // 输入框、滑条与复选框
        colors[ImGuiCol_FrameBg] = rgba(0x2A, 0x2F, 0x37);
        colors[ImGuiCol_FrameBgHovered] = rgba(0x34, 0x3A, 0x44);
        colors[ImGuiCol_FrameBgActive] = rgba(0x3E, 0x46, 0x52);

        colors[ImGuiCol_CheckMark] = accentColor();
        colors[ImGuiCol_SliderGrab] = accentColor();
        colors[ImGuiCol_SliderGrabActive] = rgba(0x52, 0x8F, 0xEA);

        // 按钮
        colors[ImGuiCol_Button] = rgba(0x2E, 0x34, 0x3E);
        colors[ImGuiCol_ButtonHovered] = rgba(0x3B, 0x43, 0x50);
        colors[ImGuiCol_ButtonActive] = accentColor();

        // 标签页与停靠预览
        colors[ImGuiCol_Tab] = rgba(0x1B, 0x1E, 0x23);
        colors[ImGuiCol_TabHovered] = rgba(0x2F, 0x36, 0x42);
        colors[ImGuiCol_TabSelected] = rgba(0x28, 0x2D, 0x36);
        colors[ImGuiCol_TabSelectedOverline] = accentColor();
        colors[ImGuiCol_TabDimmed] = rgba(0x16, 0x18, 0x1B);
        colors[ImGuiCol_TabDimmedSelected] = rgba(0x20, 0x24, 0x2B);
        colors[ImGuiCol_DockingPreview] = rgba(0x3B, 0x78, 0xD8, 0x80);

        // 表格与滚动条
        colors[ImGuiCol_TableHeaderBg] = rgba(0x24, 0x28, 0x30);
        colors[ImGuiCol_TableBorderStrong] = rgba(0x32, 0x37, 0x41);
        colors[ImGuiCol_TableBorderLight] = rgba(0x28, 0x2D, 0x35);
        colors[ImGuiCol_TableRowBg] = rgba(0x00, 0x00, 0x00, 0x00);
        colors[ImGuiCol_TableRowBgAlt] = rgba(0xFF, 0xFF, 0xFF, 0x06);

        colors[ImGuiCol_ScrollbarBg] = rgba(0x16, 0x18, 0x1B);
        colors[ImGuiCol_ScrollbarGrab] = rgba(0x34, 0x3A, 0x44);
        colors[ImGuiCol_ScrollbarGrabHovered] = rgba(0x43, 0x4B, 0x58);
        colors[ImGuiCol_ScrollbarGrabActive] = accentColor();

        // 导航与选中高亮
        colors[ImGuiCol_NavCursor] = accentColor();
        colors[ImGuiCol_TextSelectedBg] = rgba(0x3B, 0x78, 0xD8, 0x55);
    }
}  // namespace Incubator::Editor::EditorTheme
