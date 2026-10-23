#pragma once

#include <array>
#include <cstddef>

#include "runtime/platform/input/input.h"

namespace Incubator::Input
{
    struct ButtonState
    {
        bool down = false;
        bool pressed = false;
        bool released = false;
    };

    struct PointerValue
    {
        double x = 0;
        double y = 0;
        bool operator==(const PointerValue&) const = default;
    };

    // Owned raw input snapshot. Event accumulation is independent of GLFW polling and UI consumption.
    class InputState
    {
    public:
        void beginFrame() noexcept;
        void onKey(KeyCode key, KeyState action) noexcept;
        void onMouseButton(MouseButton button, KeyState action) noexcept;
        void onMousePosition(double x, double y) noexcept;
        void onScroll(double x, double y) noexcept;
        void onFocus(bool focused) noexcept;

        ButtonState key(KeyCode key) const noexcept;
        ButtonState mouseButton(MouseButton button) const noexcept;
        PointerValue mousePosition() const noexcept
        {
            return position_;
        }
        PointerValue mouseDelta() const noexcept
        {
            return delta_;
        }
        PointerValue scroll() const noexcept
        {
            return scroll_;
        }
        bool focused() const noexcept
        {
            return focused_;
        }
        // Remains true even if focus returns within this input round.
        bool focusLost() const noexcept
        {
            return focusLost_;
        }

    private:
        std::array<ButtonState, static_cast<std::size_t>(KeyCode::World2) + 1> keys_{};
        std::array<ButtonState, static_cast<std::size_t>(MouseButton::X2) + 1> buttons_{};
        PointerValue position_;
        PointerValue delta_;
        PointerValue scroll_;
        bool focused_ = true;
        bool focusLost_ = false;
        bool positioned_ = false;
    };
}  // namespace Incubator::Input
