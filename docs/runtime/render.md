# Render 模块设计

## 目标与范围

Render 是 IncubatorRuntime 中的 Function 层模块，负责把应用准备的绘制数据提交到图形设备。第一版采用 Vulkan 具体实现，不建立多后端 RHI、通用继承体系或插件框架。

当前执行基线：

- Vulkan 1.3；设备必须支持并启用 Dynamic Rendering 与 Synchronization2。
- Windows、单窗口、主线程录制；上传和绘制使用同一图形队列。
- 显存分配器在任务 05 开始前另行审核，本模块当前不引入 VMA。

任务 02 的验证模式在初始化配置中选择 `Disabled`、`Optional` 或 `Required`。Demo 的 Debug 构建采用 `Optional`：缺少 Khronos Validation Layer 或 `VK_EXT_debug_utils` 时记录告警并继续；`Required` 缺少任一组件时初始化失败。Release Demo 默认禁用 Validation。

`VulkanInstance` 请求 Vulkan 1.3，并在创建前确认 Loader 版本及 GLFW 必需扩展。`GlfwSurface` 借用 `Window` 创建 Surface。`VulkanDevice` 按枚举顺序选择首个满足 API 1.3、`VK_KHR_swapchain`、图形与呈现队列、非空 Surface 格式/呈现模式，以及 Dynamic Rendering 和 Synchronization2 的物理设备；图形与呈现队列族相同时优先采用该组合，否则分别保存两个队列族。逻辑设备启用并输出这些必需功能信息。

## 模块依赖

Render 可使用 Core、Platform、Resource 和 Vulkan；Core、Platform、Resource 不依赖 Render。GLFW 与 Vulkan Surface 的连接属于 Render，`Platform::Window` 不添加 Vulkan 创建接口。公共渲染头文件不暴露 Vulkan 类型，Runtime 对 Vulkan 的链接保持私有。

源码归入 `src/runtime/render/`，应用入口位于该目录，实现放在 `render/vulkan/`。文件由现有 `IncubatorRuntime` 目标的 `src/runtime/CMakeLists.txt` 收集，不新增渲染库。

## 对象职责与所有权

| 对象 | 职责与所有权 |
|---|---|
| `Renderer` | 由应用创建和持有；借用 `Window`；组合并拥有本次渲染所需内部对象；提供初始化、完整帧绘制和关闭入口。 |
| `VulkanInstance` | 创建并拥有 Vulkan Instance 和可选 debug messenger；查询并启用 GLFW 所需实例扩展；销毁晚于 Surface。 |
| Surface | `GlfwSurface` 由 Render 通过 GLFW 从借用的 Window 创建和销毁；依赖 Instance，Window 必须活到 Surface 销毁之后。 |
| `VulkanDevice` | 选择支持图形、呈现、Swapchain、Dynamic Rendering 和 Synchronization2 的首个物理设备；创建并拥有逻辑设备和队列；销毁前先清理设备子对象。 |
| `VulkanSwapchain` | 拥有交换链及图像视图等呈现资源；依赖 Device 与 Surface。 |
| `VulkanFrameScheduler` | 管理帧槽、命令录制与提交同步；依赖 Device，并协调 Swapchain 图像生命周期。 |

初始化依赖顺序为 Instance → Surface → Device → Swapchain/帧资源 → 绘制资源；关闭时按反向依赖销毁。Renderer 的对象成员和显式关闭顺序必须保证 Surface 先于 Instance 销毁、所有设备资源先于 Device 销毁，并由应用保证 Window 活到 Renderer 完成关闭。

## 调用与错误约定

应用继续拥有主循环，负责事件轮询并准备本帧 CPU 绘制数据。Renderer 一次调用负责获取图像、录制命令、提交和呈现；调用期间消费 CPU 数据，不缓存到下一帧。应用不接触已获取 GPU 帧的取消与补救同步规则。

窗口尺寸为零或交换链暂不可绘制属于正常跳帧，由 Renderer 以非错误结果结束本次调用，不保留本帧绘制数据。初始化失败、必需功能不支持或不可恢复的 Vulkan 操作失败属于渲染错误，沿用 `Error::Exception` 并使用 `Error::Code::RENDER`，通过现有 Log 上报；不吞掉错误后继续运行。

ImGui 接入后，应用通过 UI 适配层开始 UI 帧、构建界面并结束 UI 帧以产生本帧 UI 绘制数据；适配层负责 ImGui 后端和 UI 命令录制，Renderer 在同一完整帧调用中消费数据并完成提交与呈现。UI 数据同样仅在本次调用期间有效。

## 后续实施边界

本任务只固定模块位置、依赖、所有权、调用职责和错误分类。具体 Vulkan 对象与帧实现随任务 02–03 加入；GPU 分配策略留待任务 05 审核。无需预先创建空类或占位实现。
