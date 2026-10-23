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

    /// @brief 保存一帧内累计的原始输入状态。
    /// @note 事件累计不依赖 GLFW 轮询，也不受 UI 消费影响。
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
        /// @brief 查询本轮输入中是否发生过焦点丢失。
        /// @return 本轮发生过焦点丢失时返回 true，即使焦点随后恢复也不清除此标记。
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
