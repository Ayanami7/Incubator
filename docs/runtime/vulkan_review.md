# Vulkan 系列最终复核

复核日期：2026-10-13。逐项按 R-07、R-08、R-02、R-06、R-04、R-01、R-03、R-05 执行，分别构建、测试和提交。提交不推送远端。

构建遵循 WORKFLOW.md：通过 VS 2022 VsDevCmd 初始化 x64 环境后，执行 Debug / all 构建。沙盒内出现 MSVC C1902，沙盒外同一工作流构建成功；后续验收使用沙盒外构建。日志存于本地忽略目录 `build/review/<条目>/`。当前测试仅使用从源码重新构建的目标，旧构建目录中的无源文件可执行文件不计为证据。

## R-07 文档实施状态

- 可行性：不依赖 GPU，现有代码与任务 05 已明确原生逐资源分配，适合直接统一文档。
- 变更：Render 文档对齐任务 01–08 已实施能力、原生内存分配和后续范围；保留实施与运行验收的区别。任务总览的 GPU 内存决策已采用已实施状态，任务 05 的历史验证状态保留。
- 验证：基于 `4977c7b` 加本条文档修订，Debug / all 构建通过，现有 14 个 Runtime 测试可执行文件全部通过（退出码 0）。本条无需 GPU 验收，不将该结果推广为后续 Vulkan 条目通过。

## R-08 Loader 异常契约

- 可行性：Manager 已提供统一异常门面，无需改变 Loader 行为或增加捕获层。CPU 加载可独立验证。
- 变更：统一 LoaderBase 与 Loader 文档；增加 ShaderLoader 成功加载/卸载、直接异常与门面单次日志、格式失败后重试测试。启用顶层 CTest 和测试文件自动发现。
- 验证：基于 `9fc555a` 加本条修订，Debug / all 构建通过；ShaderLoader、TextLoader、ResourceManager 三个测试目标全部通过。覆盖正常 nullptr、异常失败、单次诊断、载荷挂载/卸载和失败后重试。

## R-02 目标设备与扩展基线

- 可行性：用户已确认本机 Windows + RTX 5070 Ti 为本轮目标，保留 maintenance1 单一路径；不增加兼容实现。
- 变更：补全最低实例/设备扩展与 feature 要求及不支持时的诊断；新增 Required Validation 的实际设备能力、绘制/resize/关闭测试。
- 验证：基于 `69ff53e` 加本条修订，Debug / all 构建和两个 GPU 测试通过，Required Validation 实际启用，warning/error 为零。设备 NVIDIA GeForce RTX 5070 Ti，API 1.4.325，driverVersion 2480242688，graphics/present family 均为 0；Surface minImageCount=2、maxImageCount=8。
- 运行：验证必要实例扩展、设备扩展和三个 feature；绘制场景与 UI 共 20 帧，连续四次尺寸配置后自动重建并正常关闭。该目标满足当前要求，不推广为其他硬件兼容结论。

## R-06 首次零尺寸与 UI 就绪

- 可行性：首次零尺寸的缺口可在 Renderer/UI 边界修正，不需要通用状态机。GLFW 不允许创建 0x0 窗口，因此测试用 Window 装饰器仅控制报告的 framebuffer 可用性，底层 Surface、设备和绘制仍真实运行；另测试真实窗口最小化/恢复。
- 变更：`Renderer::beginUiFrame()` 先准备交换链/后端/字体，失败则不开始 ImGui 帧。Demo 和测试遵循返回值；恢复时先初始化后端，避免使用未构建字体或旧纹理标识的 UI 数据。交换链零 extent 仍保留重建请求。
- 验证：基于 `b99cf04` 加本条修订，Debug / all 构建及四个 Vulkan 测试通过。Required Validation 下，首次报告零 framebuffer 连续五帧不生成 UI，恢复原尺寸后自动开始绘制；真实可见窗口四轮 resize、最小化/恢复并关闭通过，无 warning/error。未强制模拟 Surface currentExtent 与 framebuffer 异步不一致的驱动特例。

## R-04 ImGui 关闭顺序

- 可行性：现有关闭顺序已具有对应等待条件，适合直接做真实 GPU 回归，无需增加重复等待。
- 静态依据：ImGui 适配器析构时 Device 仍存活，`vkDeviceWaitIdle` 完成引用字体/描述符/绘制缓冲的 GPU 工作后才关闭 backend；调度器随后等待 present fence，回收交换链和呈现信号量。参见 [Device idle](https://docs.vulkan.org/refpages/latest/refpages/source/vkDeviceWaitIdle.html) 与 [present fence](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html)；present fence 提供资源回收依据，并不保证等待实际显示呈现结束。
- 验证：基于 `37fabdf` 加本条回归测试，Debug / all 构建通过。Required Validation 下 12 次创建 Renderer、提交 1–3 个场景/UI 帧后立即销毁，测试未额外等待；所有轮次通过，warning/error 为零。

## R-01 日志诊断与频率

- 可行性：现有创建/状态日志覆盖了字段要求，可通过实际绘制、窗口尺寸变化和 debug messenger 消息验证；不增加日志去重系统。
- 变更：增加正常帧/跳帧日志频率、获取图像后 resize 恢复与诊断消息传递测试；重建原因文字统一为 status recovery。
- 验证：基于 `d72a04c` 加本条修订，Debug / all 构建和三个测试通过。20 个正常帧仅一次创建日志；resize 仅一次对应重建日志；零 framebuffer 的连续跳帧无新增日志。获取图像后 resize 实际触发 present OUT_OF_DATE，下一帧重建为 710x430 并恢复 UI，Required Validation 无非预期 warning/error。
- 诊断测试：两条明确标记的注入 warning/error 均进入日志，属于预期测试消息；SUBOPTIMAL 本轮未由驱动返回，静态保留其 acquire/present 日志分支。
