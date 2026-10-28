# Input 模块设计

## 目标与组织

Input 提供 GLFW 常量到引擎枚举的转换，以及由窗口适配填充的自有 `InputState`。
状态算法不调用 GLFW、Render 或 ImGui；Window 已通过实际回调填充本实例状态；Demo 的批次适配见下方说明。

- `input.h`：已有 KeyCode、MouseButton、KeyState 和内联 GLFW 转换函数。
- `input_state.h/.cpp`：按轮次累计原始状态，可按值复制，无全局单例。
- 命名空间：`Incubator::Input`。

## 按轮次状态

```cpp
InputState input;
input.beginFrame(); // Once per input round, before event polling.
input.onKey(KeyCode::F, KeyState::Press);
input.onMousePosition(100, 200);
input.onScroll(0, 1);
const ButtonState f = input.key(KeyCode::F);
const auto snapshot = input; // Independent owned copy.
```

| 接口 | 约定 |
|---|---|
| `beginFrame()` | 清理 pressed/released、mouseDelta、scroll 和 focusLost，保留 down、位置及当前焦点 |
| `onKey` / `onMouseButton` | 接收引擎枚举与 KeyState；Unknown/越界枚举忽略 |
| `key` / `mouseButton` | 返回 ButtonState 值：down、pressed、released；未知查询返回全 false，不消费状态 |
| `onMousePosition(x,y)` | 窗口坐标 double；首次定位只建基准，后续差值累计至 mouseDelta |
| `onScroll(x,y)` | 累加水平/垂直滚轮增量，scroll() 返回本轮总量 |
| `mousePosition()` / `mouseDelta()` / `scroll()` | 返回 PointerValue{x,y} 副本，位置与增量沿用窗口坐标单位 |
| `onFocus(bool)` / `focused()` | 初始聚焦为 true；窗口适配创建时应同步实际焦点，失焦时清理按住状态 |
| `focusLost()` | 本轮曾失焦；即使同轮恢复仍为 true，供消费者取消旧手势 |

Press 仅在从未按住转为按住时设置 pressed；Release 仅在结束按住时设置 released。
同轮 press/release 两种边沿均保留，最终 down=false；随后再 press 时 down=true，两种边沿仍为 true。
Repeat 不产生 pressed，也不在缺少首次按下时合成按住。重复 Release 不产生额外边沿。
这些是汇总查询，不能还原同轮事件顺序、每次按下位置或 ImGui 的消费批次；UI 归属协调属于后续适配任务。

失焦产生所有当前按住键/按钮的 released，清理 pressed、鼠标位移和滚轮，重置定位基准。
未聚焦期间忽略按键、按钮、位置和滚轮输入；恢复不自动恢复 down，首次定位无历史跳变。
额外事件累积不清理状态，只有调用方显式 beginFrame 才开始新一轮。Window 接入后应每轮调用一次，
额外 pollEvents 不得隐式调用 beginFrame。消费者取得 const 视图或值副本；查询不消费输入。

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

1. InputState 存储原始输入状态；窗口接入、UI 捕获与相机手势属于各自适配和消费层。
2. Input 模块**不定义动作映射**（如"跳跃 = Space"）。动作映射属于消费者或后续动作系统，Input 仅提供原始枚举与状态
3. 转换函数对所有未知输入返回 `Unknown` / `Release`，不会抛出异常或崩溃
4. 未来切换输入后端（如 SDL）时，只需新增转换函数（如 `fromSdlKey`），KeyCode 枚举无需修改
5. KeyCode 的有效枚举范围为 0–120；InputState 在访问内部数组前检查索引范围。

## Demo 与 ImGui 消费批次

DemoApp 每轮执行 beginInputFrame → pollEvents → update → beginUiFrame。
`Callbacks::cancelInteraction` 在无有效 extent、当前失焦或本轮曾失焦时调用，即使 UI 被跳过也执行。
`Callbacks::input(DemoApp&, FrameExtent, const UiInput&)` 在有效 NewFrame 后、面板和帧提取前调用，
提供本次实际 extent。ModelScene 已消费此入口实现轨道旋转、平移、缩放及 F 聚焦；取消通知直接结束旧手势。

`examples/demo/ui_input.h` 的 UiInputAdapter 读取 ImGui 本次已消费的鼠标右/中键边沿、位置/位移、
滚轮和 F 键首次按下，配合同一次 NewFrame 的捕获标记；不混用原始轮次 pressed 和 UI 捕获。
保持默认 ConfigInputTrickleEventQueue，延迟边沿随 ImGui 消费批次保留。新手势消费者应以按下位置
建立基准，忽略开始之前的位移；捕获标记只决定新手势归属，释放不受屏蔽。已有手势的位移与释放同批时先应用末段位移，再结束手势；显式取消则直接丢弃旧操作。

取消后适配器持续丢弃操作，直到 ImGui 待处理队列排空，且原始状态与 ImGui 的右/中键和 F 均释放。
中性帧本身不产生操作，此后的新合法按下才能开始交互。该恢复边界内的按下也被丢弃，需释放后重按；
不重放最小化/失焦期间的操作，不清空 ImGui 的 UI/文本事件。
仅为判断队列是否排空，示例适配层读取固定 ImGui 1.91.9b-docking 的内部 InputEventsQueue；升级时须复核此依赖。
Runtime Input 和 Window 不依赖 ImGui，原始状态仍独立可读且不被 UI 消费改写。
