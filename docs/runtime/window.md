# Window 模块设计

## 目标

Window 模块封装 GLFW 窗口的创建与生命周期管理，通过抽象类提供可 Mock 的平台无关接口。上层代码通过 `createPlatformWindow()` 工厂函数获取窗口实例，无需关心底层窗口库细节。它不处理事件分发（留给未来的 Event 模块），也不涉及渲染集成（留给未来的 Function 模块）。

## 设计原则

1. **封装性**：GLFW 细节完全隐藏于 `WindowGlfw` 内部类，抽象接口仅暴露纯虚方法
2. **可测试性**：`Window` 为抽象基类，测试时可注入 Mock 实现，无需真实 GLFW 窗口
3. **平台无关接口**：通过 `getNativeHandle()` 按需暴露原生句柄，常规使用不依赖平台 API
4. **RAII**：窗口创建和销毁由 `std::unique_ptr<Window>` 自动管理，析构函数负责清理

## 命名空间

```cpp
namespace Incubator::Window
```

## 职责边界

| 层次 | 模块 | 职责 |
|------|------|------|
| **Platform** | `Window` | 窗口创建、轮询（`pollEvents()`）、关闭检查、尺寸查询、标题设置、原生句柄暴露 |
| **Core**（未来） | `Event` | 事件分发：将 GLFW 回调中的原始事件转换为引擎事件对象并传递给订阅者 |
| **Function**（未来） | `Render` / `ImGui` | Vulkan surface 创建、ImGui 初始化、渲染循环管理 |

本模块**不会**实现：
- 事件分发与回调注册（`pollEvents()` 仅调用 `glfwPollEvents()`，不构造事件对象）
- InputState 或按键状态追踪
- Vulkan surface 创建或交换链配置
- ImGui 平台后端初始化

## 文件结构

| 文件 | 内容 |
|------|------|
| `src/runtime/platform/window/window.h` | `WindowDesc` 结构体、`Window` 抽象类（7 个纯虚方法）、`createPlatformWindow()` 工厂声明 |
| `src/runtime/platform/window/window.cpp` | `WindowGlfw` 匿名命名空间实现 + `createPlatformWindow()` 工厂定义 |

## 接口设计

### WindowDesc 窗口描述符

```cpp
struct WindowDesc
{
    std::string title = "Incubator";    // 窗口标题
    int32_t width = 1280;               // 窗口宽度（像素）
    int32_t height = 720;               // 窗口高度（像素）
    bool resizable = true;              // 是否允许用户调整窗口大小
    bool visible = true;                // 创建后是否立即可见
};
```

### Window 抽象类（7 个纯虚方法）

```cpp
class Window
{
public:
    virtual ~Window() = default;

    // ---- 窗口生命周期 ----
    virtual bool shouldClose() const = 0;           // 窗口是否请求关闭
    virtual void setShouldClose(bool value) = 0;    // 设置关闭请求标志

    // ---- 事件处理 ----
    virtual void pollEvents() = 0;                  // 轮询窗口事件（每帧调用）

    // ---- 尺寸查询 ----
    virtual void getFramebufferSize(int& width, int& height) const = 0;  // 帧缓冲像素尺寸
    virtual void getWindowSize(int& width, int& height) const = 0;       // 窗口屏幕尺寸

    // ---- 窗口控制 ----
    virtual void setTitle(std::string_view title) = 0;                   // 设置窗口标题
    virtual void* getNativeHandle() const = 0;                           // 获取 GLFWwindow*

    // 禁止拷贝，允许移动
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) noexcept = default;
    Window& operator=(Window&&) noexcept = default;

protected:
    Window() = default;  // 仅子类可构造
};
```

## WindowGlfw 实现

`WindowGlfw` 是 `Window` 的 GLFW 具体实现，定义于 `window.cpp` 匿名命名空间内，对外不可见。用户通过工厂函数获取实例，永远不直接操作 `WindowGlfw`。

### 初始化流程

```
构造函数(WindowDesc)
├── glfwSetErrorCallback()           // 注册 GLFW 错误回调
├── glfwInit()                       // 初始化 GLFW
│   └── 失败 → 抛出 Error::IO 异常
├── glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)  // 不创建 OpenGL 上下文
├── glfwWindowHint(GLFW_RESIZABLE, ...)           // 按 desc 设置
├── glfwWindowHint(GLFW_VISIBLE, ...)             // 按 desc 设置
└── glfwCreateWindow(...)            // 创建窗口
    └── 失败 → glfwTerminate() + 抛出 Error::IO 异常
```

### 错误回调

```cpp
void glfwErrorCallback(int error, const char* description)
{
    spdlog::error("[GLFW] Error {}: {}", error, description);
}
```

GLFW 内部错误（非致命）通过 `spdlog` 输出日志，不中断程序运行。

### 窗口参数

- 强制设置 `GLFW_CLIENT_API = GLFW_NO_API`：不创建 OpenGL 上下文，为后续 Vulkan 使用预留
- `resizable` 和 `visible` 根据 `WindowDesc` 配置

### 析构清理

```cpp
~WindowGlfw() override
{
    if (window_)
    {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
}
```

析构时自动销毁 GLFW 窗口。`glfwTerminate()` 不在析构中调用，因为可能有多个窗口共享 GLFW 实例。

### 禁止拷贝 / 允许移动

`Window` 基类已声明拷贝为 `delete`，移动为 `default`。`WindowGlfw` 作为子类仅持有 `GLFWwindow*` 指针，默认移动语义正确。

## 工厂模式

与 `FileIO` 模块相同的方式，通过工厂函数创建平台实例：

```cpp
std::unique_ptr<Window> createPlatformWindow(const WindowDesc& desc);
```

- 返回 `std::unique_ptr<Window>`，调用者拥有所有权
- 创建失败时抛出 `Incubator::Error::Exception`
- 内部实现仅一行：`return std::make_unique<WindowGlfw>(desc);`

## 使用示例

```cpp
#include "runtime/platform/window/window.h"
#include "runtime/core/error/error.h"

int main()
{
    try
    {
        // 1. 创建窗口
        Incubator::Window::WindowDesc desc;
        desc.title = "QBEngine Demo";
        desc.width = 1920;
        desc.height = 1080;
        desc.resizable = false;

        auto window = Incubator::Window::createPlatformWindow(desc);

        // 2. 主循环
        while (!window->shouldClose())
        {
            // 轮询窗口事件（必须在每帧调用）
            window->pollEvents();

            // 获取帧缓冲尺寸（用于 Vulkan 视口）
            int fbWidth, fbHeight;
            window->getFramebufferSize(fbWidth, fbHeight);

            // 渲染帧...
        }

        // 3. window 在 unique_ptr 析构时自动清理
    }
    catch (const Incubator::Error::Exception& e)
    {
        // 窗口创建失败处理
    }
}
```

### 获取原生句柄用于扩展操作

```cpp
GLFWwindow* native = static_cast<GLFWwindow*>(window->getNativeHandle());

// 例如：设置 GLFW 输入回调（未来 Event 模块会封装此操作）
glfwSetKeyCallback(native, keyCallback);
glfwSetMouseButtonCallback(native, mouseButtonCallback);
```

## 依赖

| 依赖 | 用途 | 类型 |
|------|------|------|
| `GLFW/glfw3.h` | 窗口创建、事件轮询、尺寸查询、原生句柄 | 链接库（运行时依赖） |
| `spdlog/spdlog.h` | GLFW 错误日志输出 | 链接库 |
| `runtime/core/error/error.h` | 异常类型（用于窗口创建失败时抛异常） | 内部依赖 |

## 注意事项

1. `pollEvents()` 仅调用 `glfwPollEvents()`，**不返回任何事件对象**。当前版本不包含事件系统，GLFW 回调需通过 `getNativeHandle()` 自行注册
2. 构造函数中强制设置 `GLFW_NO_API`，窗口**不创建 OpenGL 上下文**。若需 OpenGL 渲染，需修改此标志或使用单独的上下文创建接口
3. `glfwTerminate()` **不在 WindowGlfw 析构中调用**。若整个应用程序只创建单个窗口，调用方需在 `main()` 末尾手动调用 `glfwTerminate()` 释放 GLFW 全局状态
4. Window 创建失败时抛出 `Error::IO` 类型异常，调用方应使用 try-catch 处理
5. `getFramebufferSize()` 与 `getWindowSize()` 在 HiDPI 显示器上可能返回不同值：帧缓冲尺寸为实际像素，窗口尺寸为逻辑坐标。渲染相关操作应使用帧缓冲尺寸
6. 工厂函数返回 `unique_ptr`，所有权明确，不可复制，可通过 `std::move` 转移
