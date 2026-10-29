#pragma once

#include <imgui.h>
#include <imgui_internal.h>

namespace Incubator::Editor
{
    /// @brief 使用 ImGui 自带配置保存单窗口工作区布局。
    class Workspace
    {
    public:
        void reset()
        {
            reset_ = true;
        }
        void begin()
        {
            if (!initialized_)
            {
                ImGui::GetIO().IniFilename = "incubator-editor-layout.ini";
                ImGui::LoadIniSettingsFromDisk(ImGui::GetIO().IniFilename);
                initialized_ = true;
            }
            const auto id = ImGui::GetID("IncubatorEditorWorkspace");
            const bool build = reset_ || !ImGui::DockBuilderGetNode(id);
            if (build)
            {
                ImGui::DockBuilderRemoveNode(id);
                ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(id, ImGui::GetMainViewport()->WorkSize);
                auto center = id;
                const auto left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
                const auto right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
                ImGui::DockBuilderDockWindow("Objects", left);
                ImGui::DockBuilderDockWindow("Properties", right);
                ImGui::DockBuilderDockWindow("Scene", center);
                ImGui::DockBuilderFinish(id);
                reset_ = false;
            }
            ImGui::DockSpaceOverViewport(id, ImGui::GetMainViewport());
        }

    private:
        bool initialized_ = false;
        bool reset_ = false;
    };
}  // namespace Incubator::Editor
