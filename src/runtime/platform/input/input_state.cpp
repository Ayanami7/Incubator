#include "runtime/platform/input/input_state.h"

namespace Incubator::Input
{
    namespace
    {
        void updateButton(ButtonState& button, KeyState action)
        {
            if (action == KeyState::Press)
            {
                button.pressed |= !button.down;
                button.down = true;
            }
            else if (action == KeyState::Release)
            {
                button.released |= button.down;
                button.down = false;
            }
            // 重复事件会保持按下状态，但不会合成新的按下事件。
        }
    }  // namespace

    void InputState::beginFrame() noexcept
    {
        for (auto& key : keys_)
            key.pressed = key.released = false;
        for (auto& button : buttons_)
            button.pressed = button.released = false;
        delta_ = scroll_ = {};
        focusLost_ = false;
    }

    void InputState::onKey(KeyCode key, KeyState action) noexcept
    {
        const auto index = static_cast<size_t>(key);
        if (focused_ && index > 0 && index < keys_.size())
            updateButton(keys_[index], action);
    }

    void InputState::onMouseButton(MouseButton button, KeyState action) noexcept
    {
        const auto index = static_cast<size_t>(button);
        if (focused_ && index > 0 && index < buttons_.size())
            updateButton(buttons_[index], action);
    }

    ButtonState InputState::key(KeyCode key) const noexcept
    {
        const auto index = static_cast<size_t>(key);
        return index > 0 && index < keys_.size() ? keys_[index] : ButtonState{};
    }

    ButtonState InputState::mouseButton(MouseButton button) const noexcept
    {
        const auto index = static_cast<size_t>(button);
        return index > 0 && index < buttons_.size() ? buttons_[index] : ButtonState{};
    }

    void InputState::onMousePosition(double x, double y) noexcept
    {
        if (!focused_)
            return;
        if (positioned_)
        {
            delta_.x += x - position_.x;
            delta_.y += y - position_.y;
        }
        position_ = {x, y};
        positioned_ = true;
    }

    void InputState::onScroll(double x, double y) noexcept
    {
        if (!focused_)
            return;
        scroll_.x += x;
        scroll_.y += y;
    }

    void InputState::onFocus(bool focused) noexcept
    {
        if (focused_ && !focused)
        {
            for (auto& key : keys_)
            {
                key.released |= key.down;
                key.down = key.pressed = false;
            }
            for (auto& button : buttons_)
            {
                button.released |= button.down;
                button.down = button.pressed = false;
            }
            delta_ = scroll_ = {};
            positioned_ = false;
            focusLost_ = true;
        }
        focused_ = focused;
    }
}  // namespace Incubator::Input
