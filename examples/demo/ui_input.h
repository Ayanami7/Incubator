#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include "runtime/platform/input/input_state.h"

namespace Incubator::Demo
{
    struct UiInput
    {
        Input::ButtonState rotate, pan;
        Input::PointerValue position, delta, scroll;
        bool focusPressed = false;
        bool captureMouse = false;
        bool captureKeyboard = false;
    };

    /// @brief 从 ImGui 读取 UI 消费后的输入批次。
    /// @note 原始输入仅用于取消或重新启用交互。
    class UiInputAdapter
    {
    public:
        void cancel()
        {
            suspended_ = true;
        }

        UiInput sample(const Input::InputState& raw)
        {
            const auto& io = ImGui::GetIO();
            if (suspended_)
            {
                // 固定版本 ImGui 1.91.9b-docking：观察队列排空情况，不改变 UI 或文本事件。
                if (raw.focused() && ImGui::GetCurrentContext()->InputEventsQueue.empty() &&
                    !raw.mouseButton(Input::MouseButton::Right).down &&
                    !raw.mouseButton(Input::MouseButton::Middle).down && !raw.key(Input::KeyCode::F).down &&
                    !ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2) && !ImGui::IsKeyDown(ImGuiKey_F))
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
                    io.WantCaptureKeyboard};
        }

    private:
        bool suspended_ = false;
    };
}  // namespace Incubator::Demo
