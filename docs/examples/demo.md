# 纹理网格 Demo

`examples/demo` 演示通过 ResourceManager 加载外部 SPIR-V 和 PNG，将资源传给 Renderer，
上传应用生成的立方体或平面，绘制旋转的纹理网格并叠加 ImGui 界面。应用负责事件轮询、界面构建和主循环。

## 构建与启动

按 [开发工作流](../../WORKFLOW.md) 构建项目。本机 PowerShell 命令为：

```powershell
Set-Location 'E:/Project/VScode/Incubator'
cmd.exe /d /s /c '"D:\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && D:\CMake\bin\cmake.EXE --build e:/Project/VScode/Incubator/build --config Debug --target all --'
```

从构建目录启动：

```powershell
Set-Location 'E:/Project/VScode/Incubator/build'
& './examples/demo/demo.exe'
```

工作目录必须是所用的构建目录，资源根为该目录下的 `assets/`。
当前 Ninja 构建的入口是 `build/examples/demo/demo.exe`。源码目录不包含构建生成的 SPIR-V，
不能从源码根直接启动并期待找到这些文件。

VS Code 选择 `Debug` 后 F5，使用默认纹理；选择 `Debug (direction texture)` 则使用第二张纹理。
两份配置的工作目录均为 `${workspaceFolder}/build`。Visual Studio 生成器的 Demo 目标由
CMake 设置 `VS_DEBUGGER_WORKING_DIRECTORY=${CMAKE_BINARY_DIR}`。启动调试前先完成构建。

## 启动时选择纹理

Demo 的第一个可选位置参数表示纹理虚拟路径，不带 `assets/` 前缀：

```powershell
# 不传参数时使用 textures/reference_rgba.png。
& './examples/demo/demo.exe'
# 关闭窗口后，使用另一张图片重新启动。
& './examples/demo/demo.exe' 'textures/direction_rgba.png'
```

启动日志和 UI 显示所选路径。选择只发生在初始化阶段，换图需要重启程序。
Windows 入口使用 Unicode 命令行，再通过 `Utils::Utf8` 转为资源系统的 UTF-8 路径；
中文、空格及系统 ANSI 代码页之外的字符均可保留。路径包含空格时需像上例一样整体加引号。
两张正式样本的来源、CC0-1.0 许可、生成方式和完整像素基准见
[纹理来源说明](../../assets/textures/SOURCE.md)。

| 纹理 | 尺寸 | +Z 面方向标记 |
|---|---|---|
| `reference_rgba.png` | 4×3 | 顶行红→绿→蓝→黄，底行橙→紫→暗色→黑 |
| `direction_rgba.png` | 3×4 | 顶行白→橙→黑，次行蓝→中灰→红，底行暗色→黄→紫 |

两张图都含中灰和已知透明通道。场景材质不透明，半透明和透明格子仍显示自身 RGB。
UI 可修改清屏颜色，立方体使用固定相机并自动旋转。

## 启动时选择网格

第二个可选位置参数为 `cube` 或 `plane`；默认使用立方体。选择平面时保留第一个纹理参数：

```powershell
& './examples/demo/demo.exe' 'textures/reference_rgba.png' 'cube'
& './examples/demo/demo.exe' 'textures/reference_rgba.png' 'plane'
& './examples/demo/demo.exe' 'textures/direction_rgba.png' 'plane'
```

未知网格名会明确报告启动错误，退出码为 1。`examples/demo/scene_mesh.h` 生成立方体的
24 顶点/36 索引和平面的 4 顶点/6 索引。平面位于 XY、面向 +Z，边长 1，UV 原点在左上。
两个样本共用 `MeshVertex` position+UV、uint16 索引、同一纹理和 Pipeline。

Demo 调用 `renderer.createMesh(vertices, indices)` 同步上传，随后销毁生成的 CPU 数组；
绘制时提交返回的 `Mesh`。Renderer 持有 GPU 网格至关闭，复制网格引用不会重新上传，
缩放或相机/变换更新也不重新上传静态网格。该步骤不通过 ResourceManager 或 MODEL Loader。

## 应用帧输入

相机由 `examples/demo/scene_frame.h` 的 `makeSceneFrame(extent, draws)` 生成，
`makeSceneModel(seconds)` 使用 Demo 的 `steady_clock` 时间生成模型矩阵。Renderer 只消费最终
矩阵，不再自行旋转或生成相机；固定时间得到固定姿态。

每轮先轮询事件，调用 `beginUiFrame()`；只有返回实际绘制 extent 后才计算投影、开始 UI
内容并提交 `drawFrame(frameInput, clearColor, recordUi)`。返回空值时跳过该轮，避免零尺寸
或最小化期间计算无效宽高比。投影采用 `perspectiveRH_ZO`，应用对 Y 方向适配一次；
缩放后使用新 extent，Renderer 不再翻转。准备与提交之间若尺寸改变，本轮旧输入会被跳过。

`FrameInput.draws` 借用 `DrawItem {mesh, model}` 列表。当前 Demo 将启动所选网格放入单项列表，
应用可用相同入口提交多项或空列表；完整的多物体交互界面由后续集成任务提供。不同绘制项可
共享 Mesh，各自独立设置 model，相机和纹理共用。提交返回后可修改或销毁原始列表；
GPU 网格仍由 Renderer 持有至关闭。空列表只清屏并显示本帧 UI。

## 资产生成与部署

`demo_shaders` 使用仓库内的 `glslangValidator.exe` 编译 `assets/shaders/cube.vert` 和
`cube.frag`，目标为 GLSL 450、Vulkan 1.3、SPIR-V 1.6；输出到构建目录：

```text
assets/
├── shaders/
│   ├── cube.vert.spv
│   └── cube.frag.spv
└── textures/
    ├── reference_rgba.png
    └── direction_rgba.png
```

`assets/CMakeLists.txt` 中的 `demo_textures` 为两张 PNG 声明源文件依赖、输出和
`copy_if_different` 命令。Demo 和相关 GPU 测试共用这套部署；修改源图片后重新构建即可
更新运行资产，无需手工复制或重新链接可执行文件。PNG 已入库，普通构建不需要 Python。

使用全新构建目录时，可执行：

```powershell
Set-Location 'E:/Project/VScode/Incubator'
cmd.exe /d /s /c '"D:\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && D:\CMake\bin\cmake.EXE -S e:/Project/VScode/Incubator -B e:/Project/VScode/Incubator/build/task05-clean -G Ninja -DCMAKE_MAKE_PROGRAM=D:/ninja/ninja.exe -DCMAKE_BUILD_TYPE=Debug && D:\CMake\bin\cmake.EXE --build e:/Project/VScode/Incubator/build/task05-clean --config Debug --target demo --'
Set-Location 'E:/Project/VScode/Incubator/build/task05-clean'
& './examples/demo/demo.exe' 'textures/direction_rgba.png'
```

该启动方式的资源根是 `build/task05-clean/assets`。本机工具路径可按安装位置调整，
工作目录必须与所用构建产物对应。

## 资源持有与关闭

`examples/demo/scene_assets.h` 的 `SceneAssets` 创建 FileIO 并注册 ShaderLoader、TextureLoader，
依次取得两个 Shader 和所选纹理。清理成员在 Loader 注册前构造，覆盖后续加载以及
外层构造失败；已有登记条目显式 acquire，仅释放本次实际取得的持有。

Demo 在创建窗口前加载资源，随后借给 Renderer 构造。Renderer 返回后立即释放 CPU
资源并检查纹理已卸载，再进入绘制循环。Renderer 保留重建 Pipeline 所需的 Shader
字数组副本，并拥有已同步上传的 GPU 纹理；交换链重建不重新读取图片。

加载或 Renderer 构造失败时，SceneAssets 释放已取得的 CPU 引用、卸载归零对象，
移除 Loader，最后销毁 FileIO。正常关闭时 Renderer 先于窗口销毁。

## 验证模式与错误输入

`examples/demo/validation_mode.h` 将 Debug Demo 配置为 `Optional`，Release 配置为
`Disabled`。Optional 缺少 Khronos Validation Layer 或 `VK_EXT_debug_utils` 时会告警并继续；
需要强制启用验证的 GPU 测试显式使用 `Required`。

必需 Shader 或纹理缺失、读取或解码失败时，应用记录启动错误并返回退出码 1。
例如，在构建目录执行：

```powershell
& './examples/demo/demo.exe' 'textures/missing-demo.png'
$LASTEXITCODE
```

下面保留正确 PNG 头并截断 IDAT，固定解码器报 `PNG decode failed (outofdata)`，
应用返回 1。该反例不依赖 CRC 校验：

```powershell
$textureBytes = [IO.File]::ReadAllBytes((Join-Path (Get-Location) 'assets/textures/reference_rgba.png'))
$truncatedPath = Join-Path (Get-Location) 'assets/textures/demo-truncated.png'
try {
    [IO.File]::WriteAllBytes($truncatedPath, $textureBytes[0..71])
    & './examples/demo/demo.exe' 'textures/demo-truncated.png'
    $LASTEXITCODE
} finally {
    Remove-Item -LiteralPath $truncatedPath
}
```

## 模块说明

- [Render](../runtime/render.md)：初始化、帧调用、颜色与 GPU 生命周期。
- [Loader](../runtime/resource/loader/loader.md)：资源路径、FileIO 借用与错误语义。
- [ShaderLoader](../runtime/resource/loader/shader_loader.md)：SPIR-V CPU 载荷。
- [TextureLoader](../runtime/resource/loader/texture_loader.md)：PNG 解码与 CPU 像素约定。
- [CMake 构建结构](../cmake.md)：构建层级与通用资源部署约定。
