# Window 模块设计

## 目标

Window 模块封装 GLFW 窗口的创建与生命周期管理，通过抽象类提供可 Mock 的平台无关接口。上层代码通过 `createPlatformWindow()` 工厂函数获取窗口实例。事件对象分发留给后续 Event 模块；Vulkan Surface 和 ImGui 集成由现有 Render 模块负责，应用拥有主循环。

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
| **Function** | `Render` / `ImGui` | Vulkan Surface 创建、ImGui 后端初始化与 GPU 绘制；主循环由应用拥有 |

本模块**不会**实现：
- 事件分发与回调注册（`pollEvents()` 仅调用 `glfwPollEvents()`，不构造事件对象）
- InputState 或按键状态追踪
- Vulkan surface 创建或交换链配置
- ImGui 平台后端初始化

## 文件结构

| 文件 | 内容 |
|------|------|
| `src/runtime/platform/window/window.h` | `WindowDesc` 结构体、`Window` 抽象类、`createPlatformWindow()` 工厂声明 |
| `src/runtime/platform/window/window.cpp` | `WindowGlfw` 匿名命名空间实现 + `createPlatformWindow()` 工厂定义 |

## 接口设计

### WindowDesc 窗口描述符

```cpp
struct WindowDesc
{
    std::string title = "Incubator";    // 窗口标题
    int32_t width = 1280;               // 窗口宽度（屏幕坐标）
    int32_t height = 720;               // 窗口高度（屏幕坐标）
    bool resizable = true;              // 是否允许用户调整窗口大小
    bool visible = true;                // 创建后是否立即可见
};
```

### Window 抽象类

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

    // ---- 窗口状态查询 ----
    virtual bool isIconified() const = 0;                             // 是否最小化
    virtual float getContentScale() const = 0;                         // DPI 缩放比例（当前取 X 方向）
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
├── GLFW 引用计数 +1；首个窗口调用 glfwInit()
│   └── 初始化失败 → 计数恢复为 0，抛出 Error::Code::IO 异常
├── glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)  // 不创建 OpenGL 上下文
├── glfwWindowHint(GLFW_RESIZABLE, ...)           // 按 desc 设置
├── glfwWindowHint(GLFW_VISIBLE, ...)             // 按 desc 设置
└── glfwCreateWindow(...)            // 创建窗口
    └── 失败 → 引用计数 -1，仅减到 0 时调用 glfwTerminate()，抛出 Error::Code::IO 异常
```

### 错误回调

```cpp
void glfwErrorCallback(int error, const char* description)
{
    Log::Registry::get("platform.window").error("GLFW error {}: {}", error, description);
}
```

GLFW 内部错误（非致命）通过 `Log::Registry::get("platform.window")` 输出日志，不中断程序运行。

### 窗口参数

- 强制设置 `GLFW_CLIENT_API = GLFW_NO_API`：不创建 OpenGL 上下文，为后续 Vulkan 使用预留
- `resizable` 和 `visible` 根据 `WindowDesc` 配置

### 标题设置

`WindowDesc::title` 和 `setTitle()` 的标题采用 UTF-8 编码。`setTitle()` 按 `std::string_view` 的长度复制标题，再传给 GLFW；调用方可以传入子串、没有 NUL 终止符的字符缓冲区或空视图。

### 析构清理

```cpp
~WindowGlfw() override
{
    if (window_)
    {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    if (--s_glfwRefCount == 0)
    {
        glfwTerminate();
    }
}
```

析构时先销毁本窗口，再递减共享 GLFW 引用计数；最后一个窗口销毁时自动调用 `glfwTerminate()`。仍有其他窗口时保留 GLFW 全局状态，应用无需额外调用终止函数。

### 所有权与转移

调用方通过 `std::move(window)` 转移 `std::unique_ptr<Window>` 的所有权，底层 `WindowGlfw` 对象和原生句柄保持原位。具体实现不对外提供按值拷贝或移动入口。

## 工厂模式

与 `FileIO` 模块相同的方式，通过工厂函数创建平台实例：

```cpp
std::unique_ptr<Window> createPlatformWindow(const WindowDesc& desc);
```

- 返回 `std::unique_ptr<Window>`，调用者拥有所有权
- 创建失败时抛出 `Incubator::Error::Exception`
- 工厂构造 `WindowGlfw`，成功后查询实际窗口/Framebuffer 尺寸与缩放比例，通过 `platform.window` 记录一次 DEBUG 日志并返回所有权

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
        desc.title = "Window Example";
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
| `runtime/core/log/logger.h` | `platform.window` 创建参数与 GLFW 错误日志 | 内部依赖 |
| `runtime/core/error/error.h` | 异常类型（用于窗口创建失败时抛异常） | 内部依赖 |

## 注意事项

1. `pollEvents()` 仅调用 `glfwPollEvents()`，**不返回任何事件对象**。当前版本不包含事件系统，GLFW 回调需通过 `getNativeHandle()` 自行注册
2. 构造函数中强制设置 `GLFW_NO_API`，窗口**不创建 OpenGL 上下文**。若需 OpenGL 渲染，需修改此标志或使用单独的上下文创建接口
3. GLFW 初始化与终止由窗口引用计数管理；最后一个窗口销毁时自动终止，应用在所有借用窗口的 Renderer/Surface 销毁后再释放窗口
4. Window 创建失败时抛出 `Error::Exception`，错误码为 `Error::Code::IO`，调用方应使用 try-catch 处理
5. `getFramebufferSize()` 与 `getWindowSize()` 在 HiDPI 显示器上可能返回不同值：帧缓冲尺寸为实际像素，窗口尺寸为逻辑坐标。渲染相关操作应使用帧缓冲尺寸
6. 工厂函数返回 `unique_ptr`，所有权明确，不可复制，可通过 `std::move` 转移
