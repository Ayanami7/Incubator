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

    // Reads only the batch consumed by NewFrame. Raw input is used solely for cancellation/rearming.
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
                // Pinned ImGui 1.91.9b: observe queue drainage without altering UI/text events.
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
