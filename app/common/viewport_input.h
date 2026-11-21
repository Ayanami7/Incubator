#pragma once

#include "runtime/platform/input/input_state.h"

namespace Incubator::App
{
    /// @brief 经过 UI 消费判定后的视口输入快照（不依赖 ImGui）。
    struct UiInput
    {
        Input::ButtonState rotate, pan;
        Input::PointerValue position, delta, scroll;
        bool focusPressed = false;
        bool captureMouse = false;
        bool captureKeyboard = false;
        Input::ButtonState select;
        float selectDragDistanceSquared = 0;
    };

    /// @brief 当前场景图像在窗口逻辑坐标中的区域及交互状态。
    struct ViewportRegion
    {
        Input::PointerValue origin, size;
        bool hovered = false;
        bool focused = false;
    };

    /// @brief 将已消费的输入批次映射到场景图像；已有手势仍由控制器持有。
    /// @param input 原始输入快照。
    /// @param region 当前视口图像逻辑区域。
    /// @param textInput 当前是否有文本输入框占用键盘。
    /// @return 映射到视口坐标并附带鼠标/键盘捕获标记的输入快照。
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
}  // namespace Incubator::App
