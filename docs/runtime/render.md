# Render 模块设计

## 目标与范围

Render 是 IncubatorRuntime 中的 Function 层模块，负责把应用准备的绘制数据提交到图形设备。第一版采用 Vulkan 具体实现，不建立多后端 RHI、通用继承体系或插件框架。

当前执行基线：

- Vulkan 1.3；设备必须支持并启用 Dynamic Rendering 与 Synchronization2。
- Windows、单窗口、主线程录制；上传和绘制使用同一图形队列。
- GPU 内存采用 Vulkan 原生逐 Buffer/Image 分配；当前不引入 VMA 或自研显存池。

设备最低要求包括 Vulkan loader/设备 API 1.3、GLFW 所需 Surface 实例扩展、`VK_KHR_get_surface_capabilities2`、`VK_EXT_surface_maintenance1`，设备扩展 `VK_KHR_swapchain`、`VK_EXT_swapchain_maintenance1`，以及 `dynamicRendering`、`synchronization2`、`swapchainMaintenance1` feature 和图形/呈现队列。设备是否可用由这些能力共同决定，不能只检查 API 版本。

调用方在初始化时选择验证模式 `Disabled`、`Optional` 或 `Required`，公共 Renderer 默认采用 `Disabled`。`Optional` 在缺少 Khronos Validation Layer 或 `VK_EXT_debug_utils` 时记录告警并继续；`Required` 缺少任一组件时初始化失败。

`VulkanInstance` 请求 Vulkan 1.3，并在创建前确认 GLFW 必需扩展及 maintenance1 所需的 `VK_KHR_get_surface_capabilities2`、`VK_EXT_surface_maintenance1`。`GlfwSurface` 借用 `Window` 创建 Surface。`VulkanDevice` 按枚举顺序选择首个满足 API 1.3、`VK_KHR_swapchain`、`VK_EXT_swapchain_maintenance1`、图形与呈现队列、非空 Surface 格式/呈现模式，以及 Dynamic Rendering 和 Synchronization2 的物理设备；图形与呈现队列族相同时优先采用该组合，否则分别保存两个队列族。逻辑设备启用并输出这些必需功能信息。maintenance1 用于为呈现操作附加 Fence，确保交换链重建和关闭时可以等待呈现完成再回收旧资源。

## 模块依赖

Render 可使用 Core、Platform、Resource 和 Vulkan；Core、Platform、Resource 不依赖 Render。GLFW 与 Vulkan Surface 的连接属于 Render，`Platform::Window` 不添加 Vulkan 创建接口。公共渲染头文件不暴露 Vulkan 类型，Runtime 对 Vulkan 的链接保持私有。

模型文件由 [Model 消费层](./model.md) 准备并展开为同一 FrameInput/DrawItem，Renderer 不解析文件或遍历节点。
Scene 消费者使用 [SceneFrame](./scene.md#场景帧提取) 持有 extent、相机与绘制列表，提交时调用
`renderer.drawFrame(frame.input(), ...)` 临时借用当前存储；复制/移动结果后重新取得视图。
Renderer 不依赖 Scene；旧 extent 跳帧、提交内借用及过期/跨 Renderer GPU 引用拒绝边界保持。
[Scene Runtime 示例](../examples/demo.md) 由 示例内 DemoApp 统一调用这些入口；
应用可独立提供 FrameInput，GPU 所有权和在途帧同步由 Renderer 管理。
`render/detail/renderer_review.h` 仅供内部数值验证同步借用实际 GPU 网格、材质和 Pipeline；
测试附件与回读代码位于 test/support，应用使用既有 drawFrame 提交接口。

源码归入 `src/runtime/render/`，应用入口位于该目录，实现放在 `render/vulkan/`。文件由现有 `IncubatorRuntime` 目标的 `src/runtime/CMakeLists.txt` 收集，不新增渲染库。

## 对象职责与所有权

| 对象 | 职责与所有权 |
|---|---|
| `Renderer` | 由应用创建和持有；借用 `Window`；组合并拥有本次渲染所需内部对象；提供初始化、完整帧绘制和关闭入口。 |
| `Mesh` | 应用可复制的网格引用；其 GPU 顶点/索引由创建它的 Renderer 持有至关闭，引用本身不延长设备寿命。 |
| `Texture` | 应用可复制的不透明纹理引用；Image 由创建它的 Renderer 持有至关闭，引用不保留 CPU 资源或延长设备寿命。 |
| `Material` | 应用可复制的不透明材质引用；创建时复制颜色、采样参数，拥有 Sampler 并保持 Texture 依赖，材质参数与 Descriptor 不可变，由 Renderer 持有至关闭。 |
| `VulkanInstance` | 创建并拥有 Vulkan Instance 和可选 debug messenger；查询并启用 GLFW 所需实例扩展；销毁晚于 Surface。 |
| Surface | `GlfwSurface` 由 Render 通过 GLFW 从借用的 Window 创建和销毁；依赖 Instance，Window 必须活到 Surface 销毁之后。 |
| `VulkanDevice` | 选择支持图形、呈现、Swapchain、Dynamic Rendering 和 Synchronization2 的首个物理设备；创建逻辑设备和队列，查询内存类型并执行有 Fence 的一次性上传提交；销毁前先清理设备子对象。 |
| `VulkanSwapchain` | 拥有交换链及图像视图等呈现资源；依赖 Device 与 Surface。 |
| `VulkanFrameScheduler` | 管理帧槽、提交同步，并协调 Swapchain 图像生命周期；录制交由 `VulkanCommandList`。 |
| `VulkanPipeline` | 拥有图形 Pipeline 与 PipelineLayout；ShaderModule 仅在创建 Pipeline 期间存在。 |
| `VulkanCommandList` | 录制交换链图像布局转换、动态渲染清屏和绘制命令。 |
| `VulkanBuffer` | 每个 Buffer 独立申请 Vulkan 内存；支持 Host Visible 写入及带 staging 的设备本地静态上传。 |
| `VulkanImage` | 仅拥有自行创建的 Image、内存和 ImageView；交换链借用图像仍归 Swapchain 管理。 |
| `VulkanBindingSet` | 拥有共享帧/材质布局，以及帧参数专用的池与每帧槽 DescriptorSet。 |
| `VulkanMaterialBinding` | 每份材质拥有一组纹理/颜色 Descriptor、对应池和只写一次的 16 字节颜色 Uniform Buffer。 |
| `VulkanSampler` | 拥有纹理采样器；DescriptorSet 引用它和 ImageView。 |

GPU 内存首版采用 Vulkan 原生逐资源分配，不接入 VMA，也不自建显存池。`VulkanDevice` 集中选择满足 required、尽量满足 preferred 的内存类型，并提供 Fence 等待的一次性图形队列提交。静态 Buffer 和 RGBA8 Image 通过 staging 上传；Buffer 写入后按非 coherent 属性执行 flush，Image 显式从 Undefined 转为 Transfer Destination，再转为 Shader Read Only。该方案适合当前低数量基础资源，之后如资源数量和分配开销增长，再单独评估 VMA。

`VulkanFrameScheduler` 每帧槽可持有本帧命令引用的共享 GPU 资源。帧槽 Fence 确认提交完成后才清除这些引用；重建和关闭时等待在途帧完成后释放。CPU 可写 Buffer 本身不允许与 GPU 并发读写，调用方需按帧槽隔离区域或等待对应 Fence 后再写。
交换链获取信号量在颜色输出阶段等待；首个图像布局转换以同一阶段作为源，
将获取、布局转换与颜色写入连成依赖，丢弃旧内容也仍须等待获取完成。

`Renderer::drawFrame` 负责一个完整帧。帧槽（两组命令池、命令缓冲、获取信号量和提交 Fence）按帧索引轮转；present 等待信号量和呈现 Fence 按交换链图像索引分配。更新帧参数前等待对应帧槽 Fence，Frame Uniform Buffer 与深度附件按帧槽隔离。帧调度器仅管理获取、提交、呈现和资源持有；场景回调通过 `VulkanCommandList::drawIndexed` 消费网格/材质列表，随后录制 UI。帧参数 set 0 和共享 Pipeline 每次场景录制绑定一次，材质 set 1 与 model push constant 逐物体录制，不覆盖其他物体或帧槽的数据。相机、投影和模型矩阵由应用提供，Renderer 不生成场景相机或按内部时间旋转。外部纹理由 `createTexture` 同步上传，通过 Combined Image Sampler 采样；窗口重建复用网格、材质和纹理。

`Integration::ImGuiRenderer` 将仓库内 ImGui 1.91.9b 的 GLFW 与 Vulkan 后端接入 Runtime。Renderer 拥有该适配器，适配器管理 ImGui Context、GLFW 回调和 Vulkan 描述符池/字体资源；其 Vulkan 后端以 `UseDynamicRendering` 配置为当前交换链颜色和深度格式。交换链图像数量或附件格式改变时，Renderer 在等待帧和呈现完成后重新配置后端。关闭或重配置时适配器先调用 `vkDeviceWaitIdle`，再释放 ImGui Vulkan 资源。后端要求交换链至少有两个图像，数量不足时明确报渲染错误。

应用每轮先轮询事件，再调用 `renderer.beginUiFrame()`。Renderer 在此入口准备交换链、ImGui Vulkan 后端与字体图集，成功才开始 UI 帧并返回 `std::optional<FrameExtent>` 中的实际绘制尺寸；首次零尺寸、最小化或暂不可创建时返回 `std::nullopt`，应用跳过投影计算、UI 构建及绘制。成功后应用使用 ImGui API 构建界面并通过适配器调用 `endFrame()`。`drawData()` 返回仅供本次 `Renderer::drawFrame` 使用的数据；Renderer 在场景 indexed draw 后、结束同一动态渲染作用域前执行 UI 录制回调。UI 数据不跨帧保留；acquire 结果要求跳帧时不提交本轮数据，下一轮在准备后端后重新生成 UI。颜色附件使用 CLEAR 加载和 STORE 保存，因此 UI 叠加时保留场景颜色。

`ShaderResource` 拥有 CPU 字载荷；应用将资源借给 `prepareSceneResources`，Renderer 复制载荷后应用即可释放 CPU Resource。Vulkan Pipeline 创建 ShaderModule 时借用 Renderer 的字数组，Pipeline 创建完成后临时 ShaderModule 即销毁；Renderer 保留自己的 CPU 副本以支持交换链格式变化后的 Pipeline 重建。CPU Resource 的 release 不决定 GPU Pipeline 的销毁时机。首次窗口零尺寸时延后创建 Pipeline，恢复后使用已复制的字数组。

交换链按 Surface 能力选取格式和 extent，present mode 使用 FIFO；图形与呈现队列族不同则使用 concurrent sharing。帧缓冲尺寸或 Surface extent 为零时跳过绘制并保留重建请求，恢复有效尺寸后重新尝试。成功重建会记录尺寸、格式、图像数和原因；`OUT_OF_DATE` 与 `SUBOPTIMAL` 会记录状态并安排重建。重建前等待所有帧提交 Fence 及呈现 Fence，再创建新交换链并回收旧 image view、交换链图像同步对象。关闭时也等待呈现 Fence 后再销毁同步对象和交换链。Render 设备要求 `VK_EXT_swapchain_maintenance1`，以获得明确的呈现完成回收依据。

## 外部纹理与颜色约定

Renderer 通过 `createTexture(const TextureResource&, TextureMipMode = TextureMipMode::BaseLevel)`
独立上传纹理，返回 `texture.h` 中的 `Texture` 引用。每次调用同步上传为 `VK_FORMAT_R8G8B8A8_SRGB`，返回后不保留
TextureResource 指针或 CPU 像素副本，应用可在下一次绘制前 release 到零。
GPU Image、Sampler、Descriptor 由 Render 持有，在途帧另持有所用 Material 及其 Texture 依赖至 Fence 完成；
交换链重建复用这些纹理，不重新读取、解码或上传。立方体 +Z 面顶部 UV 指向 PNG 首行，
Loader、上传和 Shader 均不再翻转；默认采样仍为 nearest/repeat/基础层。
生成过 mip 的图像同样在窗口重建时复用完整链；相机缩放只影响本帧选取的 mip，
不触发上传或再次生成。精确 texel/UV 对照应显式使用 BaseLevel 与 nearest min/mag。

`TextureMipMode::BaseLevel` 保持原有单 mip 上传；显式选择 `FullChain` 时，在 GPU 上生成
完整链。每轴逐级向下除以 2、最小为 1，直到 1×1，例如 3×5 → 1×2 → 1×1，
1×9 → 1×4 → 1×2 → 1×1。1×1 仍只有一层，不执行 blit。

```cpp
auto texture = renderer.createTexture(cpuTexture, TextureMipMode::FullChain);
MaterialDesc desc{texture};
desc.sampler = {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear};
auto material = renderer.createMaterial(desc);
```

上传与采样选项彼此独立：FullChain 配默认材质仍只采基础层；单层图像配 mip 采样不会补建层级。
CPU TextureResource 继续只保存基础层，原有 64 MiB 源像素限制保持。生成使用现有 staging、
图形队列 `executeImmediate` 和逐级线性 `vkCmdBlitImage`；各级在作为源图像前建立 transfer
写到读依赖，返回前全部进入 Shader Read Only，ImageView 覆盖实际层数。没有异步生成或 CPU 回退。
RGB 按 sRGB → 线性过滤 → sRGB 写回，alpha 按线性 straight 通道过滤，不预乘；
黑白等权缩为 1×1 的 RGB 约为 188、alpha 0/255 的均值约为 128。
该转换遵循 [Vulkan blit 规则](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBlitImage.html)。

`VulkanImage::uploadRgba8` 在 staging/Image 分配前检查设备 `maxImageDimension2D`、
所选格式 optimal tiling 的 SAMPLED_IMAGE/TRANSFER_DST 功能，并查询该 2D、单 array layer、
单 sample 与实际 usage 组合的 image format limits。生成多级时才增加 TRANSFER_SRC usage，
要求 TRANSFER_SRC、BLIT_SRC、BLIT_DST 和线性过滤能力；检查 `maxMipLevels`，并将全部 mip
像素计入 `maxResourceSize` 限制。单层路径与 FullChain 的 1×1 不要求未使用的 blit 能力。
不支持时抛出 RENDER 错误，同时保留 extent/像素长度检查；Resource 不包含设备限制。
直接构造的深度附件、读回图像及后端默认上传仍为单 mip。

场景 RGB 由 sRGB 纹理采样得到 linear 值，与材质线性 RGB 相乘后，fragment Shader 使用精确 sRGB transfer function
编码后写入 UNORM，输出 alpha=1，不启用场景 blending。clearColor 与 ImGui RGB 均为
sRGB 编码值，直接写 UNORM；ImGui 沿用现有编码空间 alpha blending，不修改第三方 Shader。
交换链仅支持 `SRGB_NONLINEAR` colorSpace，按 BGRA8_UNORM、RGBA8_UNORM 顺序选择，
与设备返回的列表顺序无关。单个 UNDEFINED/SRGB_NONLINEAR 选择 BGRA8_UNORM，并检查
颜色附件支持；其他组合明确拒绝并报告候选格式与 colorSpace，不任意回退。
Swapchain 保存并暴露 colorSpace，Renderer 在初始化和重建日志中同时记录格式与 colorSpace。

初始化依赖顺序为 Instance → Surface → Device → Swapchain/帧资源 → 绘制资源；关闭时按反向依赖销毁。Renderer 的对象成员和显式关闭顺序必须保证 Surface 先于 Instance 销毁、所有设备资源先于 Device 销毁，并由应用保证 Window 活到 Renderer 完成关闭。

## 调用与错误约定

构造入口为 `Renderer(window, validationMode)`，Window 必须活到 Renderer 销毁之后。
初始化不需要 Shader、纹理或材质，空列表可直接清屏并显示 UI，不创建场景 Pipeline。
非空列表绘制前，应用显式上传纹理并准备共享场景资源：

```cpp
Renderer renderer(window, RendererValidationMode::Optional);
auto texture = renderer.createTexture(cpuTexture);
auto sharedTexture = texture; // Shares the same upload.
renderer.prepareSceneResources(vertexShader, fragmentShader);
auto material = renderer.createMaterial({sharedTexture}); // Default linear RGB is white.
auto otherMaterial = renderer.createMaterial({sharedTexture, {0.5f, 1.0f, 0.5f}});
// CPU Shader/Texture resources may now be released through ResourceManager.
```

`prepareSceneResources(vertexShader, fragmentShader)` 一次性准备共享 Shader、帧/材质布局与
相机绑定。Shader 由 Resource 层读取并验证 SPIR-V，准备入口拒绝空字数组，复制字载荷以
支持兼容 Pipeline 重建；不接收 Texture 或 Material。重复准备抛出 RENDER 错误。
准备过程先在局部创建副本、Descriptor 和可用 extent 下的 Pipeline，全部成功后才安装；
失败不留下部分准备状态，Renderer 仍可提交空帧或重新准备。Render 不通过 FileIO 或 Loader 读取资产。

Renderer 持有所有已创建纹理至关闭。复制/丢弃 Texture 引用不重复上传或提前回收；再次显式
调用 `createTexture` 则独立上传，不自动按路径或内容去重。`Texture::valid()` 表示所属
Renderer 仍持有资源，空引用及关闭后的引用无效；移动 Renderer 后引用随其内部设备保持有效。
Texture 仅标识 GPU 上传结果，与 CPU ResourceId 分别管理，也不使 Renderer 或设备延迟销毁。

## 最小材质与绑定

`material.h` 定义 `MaterialDesc { Texture texture; std::array<float, 3> color; SamplerDesc sampler; ShadingMode shading; }` 与 `Material` 引用。
纹理是必需输入，color 是有限的 0–1 线性 RGB，默认 `{1, 1, 1}`。例如：

```cpp
MaterialDesc desc{texture, {0.25f, 0.5f, 1.0f}};
auto tinted = renderer.createMaterial(desc);
desc.color = {0.0f, 0.0f, 0.0f}; // Does not change tinted.
DrawItem item{mesh, tinted, model};
```

`sampler.h` 中的 `SamplerDesc` 是不含 Vulkan 类型的值描述：`minFilter` 和 `magFilter`
使用 `TextureFilter::Nearest/Linear`，分别控制缩小和放大；`mipFilter` 使用
`MipFilter::BaseLevel/Nearest/Linear`。默认是 nearest、nearest、BaseLevel，U/V 寻址固定 Repeat。
采样字段追加在颜色之后，原有 `{texture}` 和 `{texture, color}` 初始化保持兼容。

```cpp
MaterialDesc filtered{texture};
filtered.sampler = {TextureFilter::Linear, TextureFilter::Linear, MipFilter::BaseLevel};
auto smooth = renderer.createMaterial(filtered);
filtered.sampler = {}; // Does not change smooth.
```

BaseLevel 只选择第 0 层，Nearest 选择最近 mip，Linear 在相邻 mip 之间线性过滤；
多层图像的 sampler 最大 LOD 为实际层数减一。单层图像上的各种选择均只访问第 0 层，
不自动生成或重新上传 mip。为让 min/mag 不同的基础层材质正确区分缩小与放大，后端使用
nearest mip 选择及 `maxLod=0.25`；它仍向下选择第 0 层。min/mag 相同时保持原有 `maxLod=0`。
这是 [Vulkan Sampler 规范](https://docs.vulkan.org/spec/latest/chapters/samplers.html)
规定的基础层过滤方式：直接设零会始终使用 magFilter。
创建含线性 min/mag 或实际多层线性 mip 采样的材质时检查图像格式的线性过滤支持，不支持时报 RENDER；
非法过滤枚举同样拒绝。能力查询只发生在创建阶段，不进入逐帧绘制。

颜色按线性空间相乘，因此白色 texel 乘 `{0.5, 0.5, 0.5}` 后，sRGB 编码结果约为
188/255；默认白色参数保持原纹理的中灰 128/255。材质参数不作用于清屏或 ImGui。

`createMaterial` 要求先准备共享 Shader，并在分配 GPU 资源前检查颜色和 Texture 从属关系。
空、过期、跨 Renderer 纹理，NaN、无穷或越界颜色均明确失败，不替换白图或钳制非法输入。
创建时将 RGB 复制进 16 字节 std140 `vec4 colorShading` Uniform，第四分量精确编码模式（0 为 Unlit、1 为 Lambert），不是 alpha；之后不改写该 Buffer
或 Descriptor。无论 PNG alpha 或颜色参数如何，场景输出 alpha 始终为 1，不启用透明混合。

`ShadingMode` 仅包含默认 `Unlit` 和显式 `Lambert`；旧 `{texture}`、`{texture,color}`、
`{texture,color,sampler}` 初始化保持 Unlit，非法模式在分配材质前按 RENDER 拒绝。
例如 `MaterialDesc{texture, {1,1,1}, {}, ShadingMode::Lambert}`，创建后修改描述不会改变材质。
模式与颜色共用同一参数块和固定 Pipeline，不引入 Shader 变体。

设 `B = textureLinearRGB * materialColor`，`D` 为单位光线传播方向，`N` 为单位世界法线：
Unlit 输出 `B`；Lambert 输出 `B * (ambient + lightColor * intensity * max(dot(N,-D),0))`。
计算在线性空间进行，最后 clamp 至 `[0,1]` 并使用原有精确 sRGB 编码一次，alpha 固定 1。
零法线没有直接光贡献，仍保留环境项；直接光强度与环境系数同时为零时为黑色。
这是最小漫反射预览，不包含 PBR、源灯光、阴影或背面法线翻转。

模型 Demo 显式选择 Lambert 并提供方向、线性颜色、强度与环境控件；Render 的默认材质和旧调用仍为 Unlit。

| 数据 | 绑定与更新 |
|---|---|
| 帧相机与光照 | set 0 / binding 0，160 字节 FrameData，对 vertex/fragment 可见；每个帧槽独立 Buffer/Descriptor，等待 Fence 后更新 |
| 材质纹理 | set 1 / binding 0，Combined Image Sampler；共享已有 Texture，不重复上传 |
| 材质颜色与模式 | set 1 / binding 1，16 字节 fragment Uniform；仅创建时写入 |
| 物体 model 与法线变换 | 112 字节 vertex push constant；逐物体录制 |

帧与材质布局由 Renderer 各创建一份；同一 Shader、附件格式和固定状态下，所有材质共用一份
Pipeline。新增材质只分配自己的 Sampler、参数 Buffer、DescriptorPool/Set，不复制相机数据、纹理或
Pipeline；材质不绑定某个帧槽，可以被多帧同时读取。交换链格式变化仅重建兼容 Pipeline，
不会重建材质或重新上传资源。

Renderer 持有 Material 至关闭，Material 拥有自己的 Sampler 并在内部强持有 Texture；在途帧保留所用 Material，
连带保持参数、Descriptor、Image/Sampler 有效。复制/放下应用 Material 引用不重新创建或
提前销毁 GPU 资源；`valid()`、移动及关闭后失效语义与 Mesh/Texture 一致。
关闭先等待提交和呈现，再销毁 Pipeline、材质绑定、共享布局和纹理，最后销毁设备。

每个 DrawItem 显式携带一份 Material；同一 Mesh 可搭配不同材质，不同 Mesh 可共享材质，
多个材质可共享 Texture。切换材质只需在下一帧的绘制项中选择另一份已创建引用，不改写
已有参数或 Descriptor。没有场景级默认材质，也不从上一帧保留材质选择。
空列表不要求 Shader 或 Material，继续清屏并绘制可选 UI。

## 帧提交

`frame_input.h` 定义本帧共享相机、光照与绘制列表：

| 类型/字段 | 调用方责任 |
|---|---|
| `FrameExtent { width, height }` | 使用 `beginUiFrame()` 返回的非零实际交换链 extent，不用窗口逻辑尺寸代替 |
| `CameraMatrices { view, projection }` | 提供最终 GLM 列主序矩阵；右手坐标，深度 0–1，应用对投影 Y 方向适配一次 |
| `DrawItem { mesh, material, model }` | 每项独立指定同一 Renderer 创建的网格、材质和最终模型矩阵，网格和材质均可共享 |
| `Lighting { direction, color, intensity, ambient }` | 世界空间光线传播方向、线性 RGB、直接光强度及标量环境系数，按值保存 |
| `FrameInput { extent, camera, draws, lighting }` | 相机与光照由同帧所有项共享，draws 是 `span<const DrawItem>`，允许为空；旧三字段初始化使用默认光照 |

提交签名为 `drawFrame(input, clearColor, recordUi)`，变换顺序为
`projection * view * model * position`。输入和列表只在本次调用内借用：Frame Uniform 的
view/projection 分别位于偏移 0、64，`lightDirectionIntensity`、`lightColorAmbient` 两个 vec4
分别位于偏移 128、144，总计 160 字节，符合两个 Shader 的 std140 FrameData 布局。
前者 xyz 为单位传播方向、w 为强度，后者 xyz 为线性 RGB、w 为环境系数。
Buffer 分配及 Descriptor range 覆盖该帧块；生产 drawFrame 与内部 review 入口共用打包函数。
生产提交在等待当前帧槽 Fence 后一次写入该槽 Buffer，不逐物体改写帧光照；每个 model
及其法线变换用 112 字节 vertex-stage push constant 在命令录制时复制。model 占前 64 字节，
法线矩阵为三个 16 字节对齐的列，偏移分别为 64、80、96；C++ 布局与 Shader 的 mat3 对应。
返回后调用方可修改或销毁相机、
模型与 CPU 列表，不影响已提交帧，不存在逐物体共写同一 Uniform 的覆盖。
Renderer 不额外翻转 Y，不保留输入引用，正常跳帧也不缓存上一帧输入。

`lighting.h` 中的 `Lighting` 默认传播方向 `(0,0,-1)`、白色、强度 1、环境系数 0.1。
方向必须有限且非零，允许非单位值；打包时归一化，Shader 将使用反方向作为指向光源的方向。
颜色和环境系数必须为有限的 `[0,1]`，强度必须有限且非负，0 表示关闭直接光。
非法原始 FrameInput 在获取交换链图像或录制命令前抛 RENDER，包含空绘制列表；之后可重新提交合法帧。
光照属于帧参数，不从 Scene 或 ResourceManager 读取，也不复制到 Material；Unlit 不使用光照，Lambert 使用本帧光照。

绘制列表按提交顺序完整录制，不排序或截断；没有固定物体数量上限，实际受可用内存约束。
Renderer 在获取图像和录制前解析并检查整个列表的 Mesh/Material。空、过期或跨 Renderer
引用使本次调用失败，不提交前面已检查的部分场景，不替换默认资源；之后仍可提交有效新列表。
所有物体共享 Shader、相机和 Pipeline，各自绑定材质，遮挡由现有深度测试决定。空列表仍清屏并执行
可选 UI 回调；列表数量变化不复用上一帧的物体。

提交前 Renderer 会复查可绘制状态。若交换链 extent 已与 `input.extent` 不同，跳过本次
场景及 UI 提交；下一轮重新准备并生成投影，避免旧比例用于新目标。若缩放发生在已获取图像的
命令录制期间，则按该帧已有 extent 完成提交，后续一帧准备新交换链。纹理和静态网格复用。

应用继续拥有主循环，负责事件轮询并准备本帧 CPU 绘制数据。Renderer 一次调用负责获取图像、录制命令、提交和呈现；调用期间消费 CPU 数据，不缓存到下一帧。应用不接触已获取 GPU 帧的取消与补救同步规则。

窗口尺寸为零或交换链暂不可绘制属于正常跳帧，由 Renderer 以非错误结果结束本次调用，不保留本帧绘制数据。初始化失败、必需功能不支持或不可恢复的 Vulkan 操作失败属于渲染错误，沿用 `Error::Exception` 并使用 `Error::Code::RENDER`，通过现有 Log 上报；不吞掉错误后继续运行。

ImGui 接入后，应用通过 UI 适配层开始 UI 帧、构建界面并结束 UI 帧以产生本帧 UI 绘制数据；适配层负责 ImGui 后端和 UI 命令录制，Renderer 在同一完整帧调用中消费数据并完成提交与呈现。UI 数据同样仅在本次调用期间有效。

## 应用网格与持有期

`mesh.h` 的 `MeshVertex` 依次为 `float position[3]`、`float uv[2]`、默认零的 `float normal[3]`，
紧密 32 字节，偏移分别为 0、12、20；旧两字段聚合初始化的法线为零。
Pipeline 按该类型的大小和字段偏移绑定位置、UV 和法线（location 0、1、2）。索引固定为 `uint32_t`，拓扑为不透明三角形列表。
公共输入不包含 Vulkan 类型。

normal 位于几何局部空间，可以为非单位向量；上传时以双精度长度归一化非零法线，避免其幅值导致 GPU 变换溢出，不修改调用方数组。零向量表示无可用法线，上传时不补造方向。
Renderer 从最终 `DrawItem::model` 的线性部分计算逆转置，平移不影响结果，也不另存 Scene 法线状态。
可逆仿射变换支持非均匀、负缩放及正常的小比例缩放，不用固定绝对行列式阈值判定奇异。
奇异或非仿射矩阵使用零法线变换，保留原模型矩阵和绘制；求逆结果无法有限表示时同样置零。
顶点阶段在变换后归一化非零法线并输出世界法线，保留零向量；Lambert 在片元插值后再次归一化，零法线不贡献直接光。

```cpp
// vertices/indices are application-owned vectors; Renderer must already exist.
auto mesh = renderer.createMesh(vertices, indices);
vertices.clear();
indices.clear();
auto sharedMesh = mesh; // Refers to the same upload.
std::array draws = {DrawItem{mesh, material, firstModel}, DrawItem{sharedMesh, otherMaterial, secondModel}};
FrameInput frameInput{extent, camera, draws};
// After beginUiFrame and application matrix/UI preparation:
renderer.drawFrame(frameInput, clearColor, recordUi);
```

`createMesh(span<const MeshVertex>, span<const uint32_t>)` 要求非空顶点与索引、索引数量为 3
的倍数且能表示为 uint32 绘制数量，每个索引小于顶点数量。输入在上传前集中校验，失败抛
RENDER 错误，不创建默认网格。法线的各分量必须有限，非有限值在分配和上传前拒绝；
零法线合法，位置和 UV 的具体内容由应用提供。

上传复用 VulkanBuffer 的同步 staging 路径；成功后不保留 CPU 数组或指针。Renderer
持有所有已创建网格至关闭，复制/丢弃应用的 `Mesh` 引用不重新上传或提前回收 GPU Buffer。
提交帧另持有本次各绘制项的 GPU 网格、材质及材质的纹理/参数/Descriptor 依赖至 Fence 完成；
提交调用内复制原生绑定和模型矩阵，返回后可修改或释放原 CPU 列表及材质描述。关闭时先等待提交和呈现，再释放网格，
最后销毁设备。变换更新和交换链重建只复用上传结果。

`Mesh::valid()` 表示创建它的 Renderer 仍持有该网格；空引用、已关闭 Renderer 的引用无效。
网格只能交回创建它的 Renderer，跨 Renderer 提交明确失败；移动 Renderer 后，网格随其内部
设备继续有效。应用可以保留过期引用，但不能用它绘制，引用不使设备延迟销毁。
Runtime 不生成立方体或平面，也不依赖 Demo；生成样本见 `test/support/render_demo/scene_mesh.h`。
应用集成示例见 [多物体 Demo](../examples/demo.md)：先创建两种网格，再构建 ImGui 控件，
根据本轮编辑后的状态生成相机矩阵与 0/1/2 项绘制列表；控件隐藏物体时保留已上传网格。

## 功能边界

导入模型可由 Runtime 的 [Model 准备层](./model.md) 调用现有 Mesh/Texture/Material 创建接口，
在一次准备内共享几何和纹理，并为纯色材质显式提供白纹理。应用预先准备共享 Shader，
准备层负责释放本次 CPU 持有；Renderer 不读取模型路径、查询 ResourceManager 或解析文件。
`ModelInstance` 共享准备结果，`makeModelDraws` 将实例整体变换左乘模型缓存的节点累计变换，返回普通 DrawItem
列表。应用通过同一 FrameInput 提交，列表借用、在途持有和关闭后的引用校验沿用上述约定。

当前支持共享相机与固定 Pipeline、逐物体 Mesh/Material/model 的不透明列表，以及帧级方向光和环境项驱动的 Unlit/Lambert 着色；应用控制模型与动画，可选择另一份已创建材质，不原地修改参数或纹理。渲染保持单窗口、主线程、同一图形队列上传和绘制；不提供材质文件/Loader、模型文件解析、PBR、阴影、多光源、异步资产加载、热重载、显存池、多后端或多视口。
