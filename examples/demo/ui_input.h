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

    /// @brief 当前场景图像在 ImGui 屏幕逻辑坐标中的区域及交互状态。
    struct ViewportRegion
    {
        Input::PointerValue origin, size;
        bool hovered = false;
        bool focused = false;
    };

    /// @brief 将已消费的输入批次映射到场景图像；已有手势仍由控制器持有。
    inline UiInput viewportInput(UiInput input, const ViewportRegion& region, bool textInput)
    {
        input.position.x -= region.origin.x;
        input.position.y -= region.origin.y;
        input.captureMouse = !region.hovered;
        input.captureKeyboard = !region.focused || textInput;
        if (!region.hovered)
            input.scroll = {};
        return input;
    }

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
