# Render 模块设计

## 目标与范围

Render 是 IncubatorRuntime 中的 Function 层模块，负责把应用准备的绘制数据提交到图形设备。第一版采用 Vulkan 具体实现，不建立多后端 RHI、通用继承体系或插件框架。

当前执行基线：

- Vulkan 1.3；设备必须支持并启用 Dynamic Rendering 与 Synchronization2。
- Windows、单窗口、主线程录制；上传和绘制使用同一图形队列。
- GPU 内存采用任务 05 已确定的 Vulkan 原生逐 Buffer/Image 分配；当前不引入 VMA 或自研显存池。

本轮确认的设备目标为 Windows 上的 NVIDIA GeForce RTX 5070 Ti。最低要求包括 Vulkan loader/设备 API 1.3、GLFW 所需 Surface 实例扩展、`VK_KHR_get_surface_capabilities2`、`VK_EXT_surface_maintenance1`，设备扩展 `VK_KHR_swapchain`、`VK_EXT_swapchain_maintenance1`，以及 `dynamicRendering`、`synchronization2`、`swapchainMaintenance1` feature 和图形/呈现队列。当前保持这条呈现同步路径；其他设备不因 API 版本足够而自动视为受支持。

任务 02 的验证模式在初始化配置中选择 `Disabled`、`Optional` 或 `Required`。Demo 的 Debug 构建采用 `Optional`：缺少 Khronos Validation Layer 或 `VK_EXT_debug_utils` 时记录告警并继续；`Required` 缺少任一组件时初始化失败。Release Demo 默认禁用 Validation。

`VulkanInstance` 请求 Vulkan 1.3，并在创建前确认 GLFW 必需扩展及 maintenance1 所需的 `VK_KHR_get_surface_capabilities2`、`VK_EXT_surface_maintenance1`。`GlfwSurface` 借用 `Window` 创建 Surface。`VulkanDevice` 按枚举顺序选择首个满足 API 1.3、`VK_KHR_swapchain`、`VK_EXT_swapchain_maintenance1`、图形与呈现队列、非空 Surface 格式/呈现模式，以及 Dynamic Rendering 和 Synchronization2 的物理设备；图形与呈现队列族相同时优先采用该组合，否则分别保存两个队列族。逻辑设备启用并输出这些必需功能信息。maintenance1 用于为呈现操作附加 Fence，确保交换链重建和关闭时可以等待呈现完成再回收旧资源。

## 模块依赖

Render 可使用 Core、Platform、Resource 和 Vulkan；Core、Platform、Resource 不依赖 Render。GLFW 与 Vulkan Surface 的连接属于 Render，`Platform::Window` 不添加 Vulkan 创建接口。公共渲染头文件不暴露 Vulkan 类型，Runtime 对 Vulkan 的链接保持私有。

源码归入 `src/runtime/render/`，应用入口位于该目录，实现放在 `render/vulkan/`。文件由现有 `IncubatorRuntime` 目标的 `src/runtime/CMakeLists.txt` 收集，不新增渲染库。

## 对象职责与所有权

| 对象 | 职责与所有权 |
|---|---|
| `Renderer` | 由应用创建和持有；借用 `Window`；组合并拥有本次渲染所需内部对象；提供初始化、完整帧绘制和关闭入口。 |
| `VulkanInstance` | 创建并拥有 Vulkan Instance 和可选 debug messenger；查询并启用 GLFW 所需实例扩展；销毁晚于 Surface。 |
| Surface | `GlfwSurface` 由 Render 通过 GLFW 从借用的 Window 创建和销毁；依赖 Instance，Window 必须活到 Surface 销毁之后。 |
| `VulkanDevice` | 选择支持图形、呈现、Swapchain、Dynamic Rendering 和 Synchronization2 的首个物理设备；创建逻辑设备和队列，查询内存类型并执行有 Fence 的一次性上传提交；销毁前先清理设备子对象。 |
| `VulkanSwapchain` | 拥有交换链及图像视图等呈现资源；依赖 Device 与 Surface。 |
| `VulkanFrameScheduler` | 管理帧槽、提交同步，并协调 Swapchain 图像生命周期；录制交由 `VulkanCommandList`。 |
| `VulkanPipeline` | 拥有图形 Pipeline 与 PipelineLayout；ShaderModule 仅在创建 Pipeline 期间存在。 |
| `VulkanCommandList` | 录制交换链图像布局转换、动态渲染清屏和绘制命令。 |
| `VulkanBuffer` | 每个 Buffer 独立申请 Vulkan 内存；支持 Host Visible 写入及带 staging 的设备本地静态上传。 |
| `VulkanImage` | 仅拥有自行创建的 Image、内存和 ImageView；交换链借用图像仍归 Swapchain 管理。 |
| `VulkanBindingSet` | Render 内部创建固定的 Uniform Buffer 与 Combined Image Sampler 布局、池和每帧 DescriptorSet。 |
| `VulkanSampler` | 拥有纹理采样器；DescriptorSet 引用它和 ImageView。 |

GPU 内存首版采用 Vulkan 原生逐资源分配，不接入 VMA，也不自建显存池。`VulkanDevice` 集中选择满足 required、尽量满足 preferred 的内存类型，并提供 Fence 等待的一次性图形队列提交。静态 Buffer 和 RGBA8 Image 通过 staging 上传；Buffer 写入后按非 coherent 属性执行 flush，Image 显式从 Undefined 转为 Transfer Destination，再转为 Shader Read Only。该方案适合当前低数量基础资源，之后如资源数量和分配开销增长，再单独评估 VMA。

`VulkanFrameScheduler` 每帧槽可持有本帧命令引用的共享 GPU 资源。帧槽 Fence 确认提交完成后才清除这些引用；重建和关闭时等待在途帧完成后释放。CPU 可写 Buffer 本身不允许与 GPU 并发读写，调用方需按帧槽隔离区域或等待对应 Fence 后再写。

`Renderer::drawFrame` 负责一个完整帧。帧槽（两组命令池、命令缓冲、获取信号量和提交 Fence）按帧索引轮转；present 等待信号量和呈现 Fence 按交换链图像索引分配。更新帧变换前等待对应帧槽 Fence，Uniform Buffer 与深度附件按帧槽隔离；`VulkanCommandList` 录制颜色/深度布局、动态渲染、DescriptorSet 绑定、立方体 indexed draw 和呈现布局。固定相机使用 GLM，模型每帧旋转；程序生成的棋盘纹理通过 Combined Image Sampler 采样。窗口尺寸变化会重建匹配 extent 的深度附件并更新投影，交换链颜色格式变化时重建兼容 Pipeline。

`Integration::ImGuiRenderer` 将仓库内 ImGui 1.91.9b 的 GLFW 与 Vulkan 后端接入 Runtime。Renderer 拥有该适配器，适配器管理 ImGui Context、GLFW 回调和 Vulkan 描述符池/字体资源；其 Vulkan 后端以 `UseDynamicRendering` 配置为当前交换链颜色和深度格式。交换链图像数量或附件格式改变时，Renderer 在等待帧和呈现完成后重新配置后端。关闭或重配置时适配器先调用 `vkDeviceWaitIdle`，再释放 ImGui Vulkan 资源。后端要求交换链至少有两个图像，数量不足时明确报渲染错误。

Demo 每轮先轮询事件，再调用 `renderer.imgui().beginFrame()`、使用 ImGui API 构建界面并调用 `endFrame()`。`drawData()` 返回仅供本次 `Renderer::drawFrame` 使用的数据；Renderer 在场景 indexed draw 后、结束同一动态渲染作用域前执行 UI 录制回调。UI 数据不跨帧保留；窗口尺寸为零或 acquire 结果要求跳帧时，回调不执行，下一轮由应用生成新 UI 数据。颜色附件使用 CLEAR 加载和 STORE 保存，因此 UI 叠加时保留场景颜色。

Demo 构建时调用仓库内的 `glslangValidator.exe`，将 `assets/shaders/cube.vert` 和 `.frag` 编译到构建目录的 `assets/shaders/`，目标为 GLSL 450、Vulkan 1.3 与 SPIR-V 1.6。Demo 注册 `shaders/cube.vert.spv` 与 `shaders/cube.frag.spv` 两个 `ResourceType::SHADER`，由 `ShaderLoader` 按资源根约定映射到构建目录的 `assets/shaders/`，并通过 `Resource::Loader::readSpirv` 读取和验证。`ShaderResource` 拥有 CPU 字载荷；Demo 将资源借给 Renderer 构造，Renderer 复制载荷后 Demo 即可释放 CPU Resource。Vulkan Pipeline 创建 ShaderModule 时借用 Renderer 的字数组，Pipeline 创建完成后临时 ShaderModule 即销毁；Renderer 保留自己的 CPU 副本以支持交换链格式变化后的 Pipeline 重建。CPU Resource 的 release 不决定 GPU Pipeline 的销毁时机。

交换链按 Surface 能力选取格式和 extent，present mode 使用 FIFO；图形与呈现队列族不同则使用 concurrent sharing。帧缓冲尺寸或 Surface extent 为零时跳过绘制并保留重建请求，恢复有效尺寸后重新尝试。成功重建会记录尺寸、格式、图像数和原因；`OUT_OF_DATE` 与 `SUBOPTIMAL` 会记录状态并安排重建。重建前等待所有帧提交 Fence 及呈现 Fence，再创建新交换链并回收旧 image view、交换链图像同步对象。关闭时也等待呈现 Fence 后再销毁同步对象和交换链。Render 设备要求 `VK_EXT_swapchain_maintenance1`，以获得明确的呈现完成回收依据。

初始化依赖顺序为 Instance → Surface → Device → Swapchain/帧资源 → 绘制资源；关闭时按反向依赖销毁。Renderer 的对象成员和显式关闭顺序必须保证 Surface 先于 Instance 销毁、所有设备资源先于 Device 销毁，并由应用保证 Window 活到 Renderer 完成关闭。

## 调用与错误约定

应用继续拥有主循环，负责事件轮询并准备本帧 CPU 绘制数据。Renderer 一次调用负责获取图像、录制命令、提交和呈现；调用期间消费 CPU 数据，不缓存到下一帧。应用不接触已获取 GPU 帧的取消与补救同步规则。

窗口尺寸为零或交换链暂不可绘制属于正常跳帧，由 Renderer 以非错误结果结束本次调用，不保留本帧绘制数据。初始化失败、必需功能不支持或不可恢复的 Vulkan 操作失败属于渲染错误，沿用 `Error::Exception` 并使用 `Error::Code::RENDER`，通过现有 Log 上报；不吞掉错误后继续运行。

ImGui 接入后，应用通过 UI 适配层开始 UI 帧、构建界面并结束 UI 帧以产生本帧 UI 绘制数据；适配层负责 ImGui 后端和 UI 命令录制，Renderer 在同一完整帧调用中消费数据并完成提交与呈现。UI 数据同样仅在本次调用期间有效。

## 后续实施边界

任务 01–08 已实现设备与交换链管理、动态渲染、纹理立方体和深度绘制、ImGui 叠加，以及 ResourceManager 的 SPIR-V CPU 加载链路。Buffer/Image 各自拥有 Vulkan 原生内存分配，帧调度器负责提交完成后的资源释放；实现状态不等于 GPU/Validation 验收通过，最终复核结果见 [Vulkan 复核记录](./vulkan_review.md)。

后续范围包括按实际需求评估纹理文件解码、异步加载、热重载、Cook 与资产清单。当前保持单窗口、主线程、同一图形队列上传和绘制；不扩展通用帧状态机、帧取消协议、显存池、多后端或多视口。若资源规模或分配开销成为问题，再另行评估 VMA。
