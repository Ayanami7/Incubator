# ShaderLoader 与 ShaderResource

## 目标

通过已有 `ResourceManager` 同步加载预先编译的 SPIR-V，形成
`虚拟路径 → ResourceId → ShaderLoader → ShaderResource → Renderer → VulkanPipeline` 链路。

## 资源路径与读取

调用方将 SPIR-V 虚拟路径注册为 `ResourceType::SHADER`。Loader 按 `Path::join("assets", virtualPath)` 得到相对路径，通过 `Utils::Utf8::toFsPath` 转换后交给 FileIO。SPIR-V 长度、字对齐和 magic 检查只在 `readSpirv` 二进制读取入口执行；ShaderLoader 不复制读文件或格式验证逻辑。`readSpirv` 的异常诊断使用 `Utils::Utf8::toUtf8String` 输出物理路径。运行时不调用编译器，调用方应预先提供编译产物。

资源根是启动工作目录下的 `assets/`。调用方负责部署文件并在有效持有或缓存对象存在期间保持工作目录稳定。

## CPU 与 GPU 所有权

- ShaderResource 拥有 `readSpirv` 返回的 `std::vector<uint32_t>`，仅包含 CPU 侧代码。
- 调用方通过 ResourceManager 获取资源并传给 Renderer。Renderer 在构造时复制载荷，成功返回后调用方可释放 ResourceId 的引用；Renderer 的副本用于后续 Pipeline 重建。
- VulkanPipeline 在创建阶段借用 CPU 字数组填写 `VkShaderModuleCreateInfo`。创建 Pipeline 后立即销毁临时 ShaderModule；Vulkan Pipeline 不引用 CPU 字数组。
- Renderer 持有 CPU 字数组直到 Renderer 销毁。GPU Pipeline 与其他 GPU 资源按 Renderer/帧调度器的 Vulkan 生命周期销毁，ResourceManager 的 CPU 引用计数不控制 GPU 退役。

## 失败语义

文件缺失或 SPIR-V 格式错误由 `readSpirv` 抛出 I/O 异常，再由 ResourceManager 的 `load()` 门面记录并转换为 `nullptr`。调用方必须检查加载结果，不能将空资源传给 Renderer。
