# ShaderLoader 与 ShaderResource

## 目标

通过已有 `ResourceManager` 同步加载 CMake 编译生成的 SPIR-V，形成
`虚拟路径 → ResourceId → ShaderLoader → ShaderResource → Renderer → VulkanPipeline` 链路。

## 资源路径与读取

Demo 将 `shaders/cube.vert.spv` 和 `shaders/cube.frag.spv` 注册为 `ResourceType::SHADER`。Loader 按现有资源根约定将 UTF-8 虚拟路径解析为 `assets/shaders/<name>.spv`，通过 `Utils::Utf8::toFsPath` 转换后交给 FileIO。SPIR-V 长度、字对齐和 magic 检查只在该二进制读取入口执行；ShaderLoader 不复制读文件或格式验证逻辑。`readSpirv` 的异常诊断使用 `Utils::Utf8::toUtf8String` 输出物理路径。编译仍由 Demo 的 CMake shader target 完成，运行时不调用编译器。

资源根是启动工作目录下的 `assets/`。Demo 调试和构建产物终端运行均以 `${CMAKE_BINARY_DIR}` 为工作目录，SPIR-V 输出到该目录的 `assets/shaders/`；源码树中的 Shader 输入由构建目标编译生成。完整目录关系与复现命令见 [Task 02 资源路径记录](../../../../.agents/specs/tasks/2026-10-13-texture-loading/task-02-asset-path.md)。加载期间保持工作目录稳定。

## CPU 与 GPU 所有权

- ShaderResource 拥有 `readSpirv` 返回的 `std::vector<uint32_t>`，仅包含 CPU 侧代码。
- Demo 通过 ResourceManager 获取资源并传给 Renderer。Renderer 在构造时复制载荷，随后 Demo 释放 ResourceId 的引用并清理 Loader；Renderer 的副本用于后续 Pipeline 重建。
- VulkanPipeline 在创建阶段借用 CPU 字数组填写 `VkShaderModuleCreateInfo`。创建 Pipeline 后立即销毁临时 ShaderModule；Vulkan Pipeline 不引用 CPU 字数组。
- Renderer 持有 CPU 字数组直到 Renderer 销毁。GPU Pipeline 与其他 GPU 资源按 Renderer/帧调度器的 Vulkan 生命周期销毁，ResourceManager 的 CPU 引用计数不控制 GPU 退役。

## 失败语义

文件缺失或 SPIR-V 格式错误由 `readSpirv` 抛出 I/O 异常，再由 ResourceManager 的 `load()` 门面记录并转换为 `nullptr`。Demo 将任一 ShaderResource 加载失败作为启动错误处理。
