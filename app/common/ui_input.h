#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include "viewport_input.h"

namespace Incubator::App
{
    /// @brief 从 ImGui 读取 UI 消费后的输入批次。
    /// @note 原始输入仅用于取消或重新启用交互。
    class UiInputAdapter
    {
    public:
        /// @brief 暂停向视口输出交互输入，直到原始按键与 ImGui 事件队列均已排空。
        void cancel()
        {
            suspended_ = true;
        }

        /// @brief 采样当前帧经 ImGui 消费判定后的输入状态。
        /// @param raw 平台层提供的当前窗口原始输入状态。
        /// @return 适配后的视口输入快照。
        UiInput sample(const Input::InputState& raw)
        {
            const auto& io = ImGui::GetIO();
            if (suspended_)
            {
                // 固定版本 ImGui 1.91.9b-docking：观察队列排空情况，不改变 UI 或文本事件。
                if (raw.focused() && ImGui::GetCurrentContext()->InputEventsQueue.empty() &&
                    !raw.mouseButton(Input::MouseButton::Left).down &&
                    !raw.mouseButton(Input::MouseButton::Right).down &&
                    !raw.mouseButton(Input::MouseButton::Middle).down && !raw.key(Input::KeyCode::F).down &&
                    !raw.key(Input::KeyCode::W).down && !raw.key(Input::KeyCode::E).down &&
                    !raw.key(Input::KeyCode::R).down && !raw.key(Input::KeyCode::Z).down &&
                    !raw.key(Input::KeyCode::Y).down && !ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) &&
                    !ImGui::IsMouseDown(2) && !ImGui::IsKeyDown(ImGuiKey_F) && !ImGui::IsKeyDown(ImGuiKey_W) &&
                    !ImGui::IsKeyDown(ImGuiKey_E) && !ImGui::IsKeyDown(ImGuiKey_R) && !ImGui::IsKeyDown(ImGuiKey_Z) &&
                    !ImGui::IsKeyDown(ImGuiKey_Y))
                    suspended_ = false;
                return {};
            }
            const auto button = [](int index) {
                return Input::ButtonState{ImGui::IsMouseDown(index), ImGui::IsMouseClicked(index, false),
                                          ImGui::IsMouseReleased(index)};
            };
            return {button(1),
                    button(2),
                    {io.MousePos.x, io.MousePos.y},
                    {io.MouseDelta.x, io.MouseDelta.y},
                    {io.MouseWheelH, io.MouseWheel},
                    ImGui::IsKeyPressed(ImGuiKey_F, false),
                    io.WantCaptureMouse,
                    io.WantCaptureKeyboard,
                    button(0),
                    io.MouseDragMaxDistanceSqr[0]};
        }

    private:
        bool suspended_ = false;
    };
}  // namespace Incubator::App
