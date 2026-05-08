# Input 模块设计

## 目标

Input 模块提供 GLFW 输入常量到引擎统一枚举的映射层。它将平台相关的 GLFW 整型常量（`GLFW_KEY_*`、`GLFW_MOUSE_BUTTON_*`、`GLFW_PRESS/RELEASE/REPEAT`）转换为强类型枚举，使上层代码不直接依赖 GLFW，未来切换输入后端时只需修改转换函数。

## 设计原则

1. **极简性**：仅提供枚举定义和纯转换函数，无状态、无实例
2. **无状态**：不缓存按键状态、不记录输入历史，纯粹做平台常量到引擎类型的映射
3. **易扩展**：新增按键只需在 KeyCode 枚举末尾追加项，并在转换函数中添加映射
4. **零开销**：全内联实现，头文件即可用，无运行时开支

## 命名空间

```cpp
namespace Incubator::Input
```

## 文件结构

```
src/runtime/platform/input/
└── input.h        // 枚举定义 + 转换函数（全内联实现）
```

## 接口设计

### KeyCode 键盘按键码

```cpp
enum class KeyCode : uint16_t
{
    Unknown = 0,

    // 字母 A-Z (1-26)
    A = 1, B = 2, C = 3, ..., Z = 26,

    // 数字 0-9 (27-36)
    Num0 = 27, Num1 = 28, ..., Num9 = 36,

    // 功能键 F1-F25 (37-61)
    F1 = 37, F2 = 38, ..., F25 = 61,

    // 方向与导航键 (62-71)
    Up = 62, Down = 63, Left = 64, Right = 65,
    PageUp = 66, PageDown = 67, Home = 68, End = 69,
    Insert = 70, Delete = 71,

    // 动作键 (72-77)
    Space = 72, Enter = 73, Escape = 74,
    Tab = 75, Backspace = 76, CapsLock = 77,

    // 符号键 (78-88)
    Apostrophe = 78, Comma = 79, Minus = 80, Period = 81,
    Slash = 82, Semicolon = 83, Equal = 84,
    LeftBracket = 85, RightBracket = 86, Backslash = 87,
    GraveAccent = 88,

    // 修饰键 (89-96)
    LeftShift = 89, RightShift = 90, LeftControl = 91,
    RightControl = 92, LeftAlt = 93, RightAlt = 94,
    LeftSuper = 95, RightSuper = 96,

    // 小键盘 (97-113)
    Kp0 = 97, ..., Kp9 = 106, KpDecimal = 107,
    KpDivide = 108, KpMultiply = 109, KpSubtract = 110,
    KpAdd = 111, KpEnter = 112, KpEqual = 113,

    // 锁定与系统键 (114-120)
    ScrollLock = 114, NumLock = 115, PrintScreen = 116,
    Pause = 117, Menu = 118, World1 = 119, World2 = 120
};
```

### MouseButton 鼠标按键

```cpp
enum class MouseButton : uint8_t
{
    Unknown = 0,
    Left = 1,
    Right = 2,
    Middle = 3,
    X1 = 4,   // 侧键1（后退）
    X2 = 5    // 侧键2（前进）
};
```

### KeyState 按键状态

```cpp
enum class KeyState : uint8_t
{
    Release = 0,   // 松开
    Press = 1,     // 按下
    Repeat = 2     // 长按重复
};
```

### 转换函数（全内联）

```cpp
// GLFW 按键码 -> 引擎 KeyCode
KeyCode fromGlfwKey(int glfwKey);

// GLFW 鼠标按键码 -> 引擎 MouseButton
MouseButton fromGlfwMouseButton(int glfwButton);

// GLFW 动作值 -> 引擎 KeyState
KeyState fromGlfwAction(int glfwAction);
```

## GLFW 映射表

### 键盘映射（代表性条目）

| GLFW 常量 | KeyCode | 说明 |
|-----------|---------|------|
| `GLFW_KEY_UNKNOWN` | `Unknown` | 未知按键 |
| `GLFW_KEY_A` ... `GLFW_KEY_Z` | `A` ... `Z` | 字母键（共 26 个） |
| `GLFW_KEY_0` ... `GLFW_KEY_9` | `Num0` ... `Num9` | 主键盘数字（共 10 个） |
| `GLFW_KEY_F1` ... `GLFW_KEY_F25` | `F1` ... `F25` | 功能键（共 25 个） |
| `GLFW_KEY_UP` | `Up` | 上方向键 |
| `GLFW_KEY_DOWN` | `Down` | 下方向键 |
| `GLFW_KEY_LEFT` | `Left` | 左方向键 |
| `GLFW_KEY_RIGHT` | `Right` | 右方向键 |
| `GLFW_KEY_PAGE_UP` | `PageUp` | 上翻页 |
| `GLFW_KEY_PAGE_DOWN` | `PageDown` | 下翻页 |
| `GLFW_KEY_HOME` | `Home` | Home |
| `GLFW_KEY_END` | `End` | End |
| `GLFW_KEY_INSERT` | `Insert` | 插入 |
| `GLFW_KEY_DELETE` | `Delete` | 删除 |
| `GLFW_KEY_SPACE` | `Space` | 空格 |
| `GLFW_KEY_ENTER` | `Enter` | 回车 |
| `GLFW_KEY_ESCAPE` | `Escape` | ESC |
| `GLFW_KEY_TAB` | `Tab` | Tab |
| `GLFW_KEY_BACKSPACE` | `Backspace` | 退格 |
| `GLFW_KEY_CAPS_LOCK` | `CapsLock` | 大小写锁定 |
| `GLFW_KEY_APOSTROPHE` | `Apostrophe` | ' |
| `GLFW_KEY_COMMA` | `Comma` | , |
| `GLFW_KEY_MINUS` | `Minus` | - |
| `GLFW_KEY_PERIOD` | `Period` | . |
| `GLFW_KEY_SLASH` | `Slash` | / |
| `GLFW_KEY_SEMICOLON` | `Semicolon` | ; |
| `GLFW_KEY_EQUAL` | `Equal` | = |
| `GLFW_KEY_LEFT_BRACKET` | `LeftBracket` | [ |
| `GLFW_KEY_RIGHT_BRACKET` | `RightBracket` | ] |
| `GLFW_KEY_BACKSLASH` | `Backslash` | \ |
| `GLFW_KEY_GRAVE_ACCENT` | `GraveAccent` | ` |
| `GLFW_KEY_LEFT_SHIFT` | `LeftShift` | 左 Shift |
| `GLFW_KEY_RIGHT_SHIFT` | `RightShift` | 右 Shift |
| `GLFW_KEY_LEFT_CONTROL` | `LeftControl` | 左 Ctrl |
| `GLFW_KEY_RIGHT_CONTROL` | `RightControl` | 右 Ctrl |
| `GLFW_KEY_LEFT_ALT` | `LeftAlt` | 左 Alt |
| `GLFW_KEY_RIGHT_ALT` | `RightAlt` | 右 Alt |
| `GLFW_KEY_LEFT_SUPER` | `LeftSuper` | 左 Super（Windows 键） |
| `GLFW_KEY_RIGHT_SUPER` | `RightSuper` | 右 Super |
| `GLFW_KEY_KP_0` ... `GLFW_KEY_KP_9` | `Kp0` ... `Kp9` | 小键盘数字 |
| `GLFW_KEY_KP_DECIMAL` | `KpDecimal` | 小键盘小数点 |
| `GLFW_KEY_KP_DIVIDE` | `KpDivide` | 小键盘除 |
| `GLFW_KEY_KP_MULTIPLY` | `KpMultiply` | 小键盘乘 |
| `GLFW_KEY_KP_SUBTRACT` | `KpSubtract` | 小键盘减 |
| `GLFW_KEY_KP_ADD` | `KpAdd` | 小键盘加 |
| `GLFW_KEY_KP_ENTER` | `KpEnter` | 小键盘回车 |
| `GLFW_KEY_KP_EQUAL` | `KpEqual` | 小键盘等号 |
| `GLFW_KEY_SCROLL_LOCK` | `ScrollLock` | 滚动锁定 |
| `GLFW_KEY_NUM_LOCK` | `NumLock` | 数字锁定 |
| `GLFW_KEY_PRINT_SCREEN` | `PrintScreen` | 打印屏幕 |
| `GLFW_KEY_PAUSE` | `Pause` | 暂停 |
| `GLFW_KEY_MENU` | `Menu` | 菜单键 |
| `GLFW_KEY_WORLD_1` | `World1` | 国际化键 1 |
| `GLFW_KEY_WORLD_2` | `World2` | 国际化键 2 |

### 鼠标映射

| GLFW 常量 | MouseButton | 说明 |
|-----------|-------------|------|
| `GLFW_MOUSE_BUTTON_1` | `Left` | 左键 |
| `GLFW_MOUSE_BUTTON_2` | `Right` | 右键 |
| `GLFW_MOUSE_BUTTON_3` | `Middle` | 中键 |
| `GLFW_MOUSE_BUTTON_4` | `X1` | 侧键（后退） |
| `GLFW_MOUSE_BUTTON_5` | `X2` | 侧键（前进） |
| 其他 | `Unknown` | 未知按钮 |

### 按键状态映射

| GLFW 常量 | KeyState | 说明 |
|-----------|----------|------|
| `GLFW_RELEASE` | `Release` | 按键松开 |
| `GLFW_PRESS` | `Press` | 按键按下 |
| `GLFW_REPEAT` | `Repeat` | 按键重复（长按） |
| 其他 | `Release` | 默认当作松开 |

## 使用示例

```cpp
#include "runtime/platform/input/input.h"

// GLFW 回调函数中，将 GLFW 输入常量转换为引擎类型
void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    Incubator::Input::KeyCode engineKey = Incubator::Input::fromGlfwKey(key);
    Incubator::Input::KeyState state = Incubator::Input::fromGlfwAction(action);

    if (engineKey == Incubator::Input::KeyCode::Escape &&
        state == Incubator::Input::KeyState::Press)
    {
        // 处理 ESC 按下
    }
}

// 鼠标按键同样转换
void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
{
    Incubator::Input::MouseButton engineButton =
        Incubator::Input::fromGlfwMouseButton(button);
    Incubator::Input::KeyState state =
        Incubator::Input::fromGlfwAction(action);

    if (engineButton == Incubator::Input::MouseButton::Left &&
        state == Incubator::Input::KeyState::Press)
    {
        // 处理鼠标左键按下
    }
}
```

## 依赖

- `GLFW/glfw3.h`：仅使用其中的 `GLFW_KEY_*`、`GLFW_MOUSE_BUTTON_*`、`GLFW_PRESS/RELEASE/REPEAT` 常量，均为编译时常量，**无运行时链接依赖**
- 无其他内部依赖

## 注意事项

1. Input 模块**不存储按键状态**。需要记录"当前帧哪些键被按下"属于上层 InputState 系统的职责
2. Input 模块**不定义动作映射**（如"跳跃 = Space"）。动作映射属于 Core 层，Input 仅提供原始按键枚举
3. 转换函数对所有未知输入返回 `Unknown` / `Release`，不会抛出异常或崩溃
4. 未来切换输入后端（如 SDL）时，只需新增转换函数（如 `fromSdlKey`），KeyCode 枚举无需修改
5. KeyCode 枚举值通过显式赋值锁定了数据范围（0-120），可安全地用作数组索引
