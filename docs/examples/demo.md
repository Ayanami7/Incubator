# 多物体与模型 Demo

使用 Runtime Scene 与统一应用会话的交互入口见 [Scene Runtime 示例](./scene_demo.md)，
支持会话内后续模型准备、实体创建/删除/清空及场景/Camera 替换；本页保留直接 Renderer 用法。

`examples/demo` 演示通过 ResourceManager 加载外部 SPIR-V 和 PNG，将资源传给 Renderer，
上传应用生成的立方体和平面，为每项提交独立的 Mesh、Material 和模型矩阵，共享相机并叠加 ImGui 控件。
应用负责事件轮询、场景状态、矩阵、绘制列表、界面构建和主循环。
也可选择 glTF/OBJ 模型，通过 ModelLoader、prepareModel 和 ModelInstance 展示两个共享实例。
`scene_frame.h` 将 UI 欧拉角转换为 Runtime Transform 四元数，将轨道控件转换为 Runtime Camera 世界位姿；
模型矩阵与最终相机矩阵由 [Runtime Transform/Camera](../runtime/scene.md) 生成，控件仍由 Demo 保存。

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

## 启动时选择模型

在构建目录中指定模型虚拟路径，保留模型及依赖相对于 `assets/` 的目录结构：

```powershell
& './examples/demo/demo.exe' '--model' 'models/minimal_gltf/model.gltf'
& './examples/demo/demo.exe' '--model' 'models/minimal_obj/model.obj'
```

每次启动准备一次模型，两个实例共用其 Mesh、Texture、Material 和节点元数据。
面板中的 `Instance A / Instance B` 分别控制实例整体变换及可见性；均隐藏时仍绘制背景和 UI。
`Reset scene` 恢复模型模式的初始位置、缩放和相机；修改实例、相机或窗口大小不重新上传模型。
切换文件或格式需重启。原有纹理位置参数和 `both|cube|plane` 模式继续用于程序生成几何。

正式样本采用右手、Y-up，保留原始单位与节点变换；glTF 包含非单位根、父子层级、重复几何和双材质，
OBJ 包含方向图四边形和纯色三角形。两者共用方向 PNG，纯色线性 RGB 为 `{0.5, 1, 0.5}`。
方向图原点在左上；PNG alpha 不产生透明，固定 nearest/repeat/LOD=0，无光照和背面剔除。
来源、CC0-1.0 许可及层级说明见 [模型样本](../../assets/models/SOURCE.md)，完整支持范围见
[ModelLoader](../runtime/resource/loader/model_loader.md)。

## 启动时选择纹理

Demo 的第一个可选位置参数表示纹理虚拟路径，不带 `assets/` 前缀：

```powershell
# 不传参数时使用 textures/reference_rgba.png。
& './examples/demo/demo.exe'
# 关闭窗口后，使用另一张图片重新启动。
& './examples/demo/demo.exe' 'textures/direction_rgba.png'
```

启动日志和 UI 显示所选路径。选择只发生在初始化阶段，换图需要重启程序。
Demo 当前将引擎日志阈值设为 `DEBUG`，可查看资源加载/卸载、PNG 解码尺寸、SPIR-V 字节序、
窗口尺寸与缩放、纹理与网格上传、Vulkan 参数、Pipeline 创建和 ImGui 后端初始化。日常运行可将 `main.cpp` 中
`Log::Registry::init` 的等级改为 `INFO` 并重新构建。
这些 DEBUG 日志记录低频操作，使用现有参数，不记录每帧绘制过程。
Windows 入口使用 Unicode 命令行，再通过 `Utils::Utf8` 转为资源系统的 UTF-8 路径；
中文、空格及系统 ANSI 代码页之外的字符均可保留。路径包含空格时需像上例一样整体加引号。
两张正式样本的来源、CC0-1.0 许可、生成方式和完整像素基准见
[纹理来源说明](../../assets/textures/SOURCE.md)。

| 纹理 | 尺寸 | +Z 面方向标记 |
|---|---|---|
| `reference_rgba.png` | 4×3 | 顶行红→绿→蓝→黄，底行橙→紫→暗色→黑 |
| `direction_rgba.png` | 3×4 | 顶行白→橙→黑，次行蓝→中灰→红，底行暗色→黄→紫 |

两张图都含中灰和已知透明通道。场景材质不透明，半透明和透明格子仍显示自身 RGB。
UI 可修改清屏颜色；两个物体默认同时显示，姿态固定，直到用户调整控件。

## 初始显示与网格上传

第二个可选位置参数为 `both`、`cube` 或 `plane`，只决定初始显示状态；默认 `both`。
使用此参数时保留第一个纹理参数：

```powershell
& './examples/demo/demo.exe' 'textures/reference_rgba.png' 'both'
& './examples/demo/demo.exe' 'textures/reference_rgba.png' 'cube'
& './examples/demo/demo.exe' 'textures/reference_rgba.png' 'plane'
& './examples/demo/demo.exe' 'textures/direction_rgba.png' 'plane'
```

未知选项会明确报告启动错误，退出码为 1。两种网格均在初始化时创建一次，显示开关不触发上传。
`examples/demo/scene_mesh.h` 生成立方体的
24 顶点/36 索引和平面的 4 顶点/6 索引。平面位于 XY、面向 +Z，边长 1，UV 原点在左上。
两个样本共用 `MeshVertex` position+UV、uint16 索引、同一纹理和 Pipeline，分别使用白色与线性 tint 材质。

Demo 调用 `renderer.createMesh(vertices, indices)` 同步上传，随后销毁生成的 CPU 数组；
绘制时提交返回的 `Mesh`。Renderer 持有 GPU 网格至关闭，复制网格引用不会重新上传，
缩放或相机/变换更新也不重新上传静态网格。该步骤不通过 ResourceManager 或 MODEL Loader。

## ImGui 交互

左侧 `Incubator Demo` 面板提供以下控件。三分量控件从左到右为 X、Y、Z，旋转使用角度。
拖动滑块修改数值，也可 Ctrl+单击输入；输入同样受范围限制。

| 分组 | 控件 | 范围与效果 |
|---|---|---|
| Cube / Plane | Visible | 分别显示或隐藏；均关闭时只清屏并显示 UI |
| Cube / Plane | Position | 各轴 −3～3；只平移该物体 |
| Cube / Plane | Rotation | 各轴 −180°～180°；只旋转该物体 |
| Cube / Plane | Scale | 各轴 0.1～3；只缩放该物体，保持正尺度 |
| Camera | Target | 各轴 −2～2；移动相机观察目标 |
| Camera | Yaw / Pitch | −180°～180° / −80°～80°；绕目标旋转，避开俯仰极点 |
| Camera | Distance | 2～12；调整与目标的距离 |
| Camera | FOV | 20°～80°；调整垂直视野 |
| 面板 | Clear color | 调整背景的 sRGB 颜色 |
| 面板 | Reset scene | 恢复两个物体、相机、显示开关和清屏颜色的默认值 |

可按下列步骤观察行为：

1. 默认启动，立方体位于左侧、平面位于右侧；立方体显示纹理原色，平面减少红/蓝线性分量。调整 Cube 的位置、旋转和缩放，Plane 保持原姿态和材质。
2. 调整 Plane 的同名控件，Cube 保持原姿态；两者共用当前纹理。
3. 修改 Camera 的 Yaw、Distance 或 FOV，两个物体共同受相机影响；缩放窗口后投影使用新绘制尺寸。
4. 依次关闭 Cube 和 Plane 的 Visible，物体数由 2 变为 1、0；背景和面板保留。重新开启后恢复各自编辑过的姿态。
5. 点击 Reset scene 回到默认场景；重启并选择第二张纹理，可重复相同操作。

默认相机目标为原点，Yaw=0°、Pitch=15°、Distance=5、FOV=45°。立方体位置为
(-0.8, 0, 0)，旋转为 (20°, 30°, 0°)，尺度为 1；平面位置为 (0.8, 0, 0)，无旋转，尺度为 1.3。
没有自动旋转；固定状态产生固定矩阵。物体移出视野时可通过 Reset scene 返回。

## 应用帧输入

`examples/demo/scene_controls.h` 保存应用场景状态并构建控件，`makeSceneDraws(scene, cube, plane, cubeMaterial, planeMaterial)`
按显示开关生成绘制列表。`scene_frame.h` 的 `makeSceneModel(transform)` 按
`T * Rz * Ry * Rx * S` 生成模型矩阵；`makeSceneFrame(extent, draws, camera)` 生成绕目标的
右手相机和透视投影，近/远裁面固定为 0.1/50。Renderer 只消费最终矩阵。

每轮先轮询事件，调用 `beginUiFrame()`；返回实际绘制 extent 后构建 UI，再从本轮编辑后的状态
生成矩阵和列表，提交 `drawFrame(frameInput, clearColor, recordUi)`。返回空值时跳过该轮，避免零尺寸
或最小化期间计算无效宽高比。投影采用 `perspectiveRH_ZO`，应用对 Y 方向适配一次；
缩放后使用新 extent，Renderer 不再翻转。准备与提交之间若尺寸改变，本轮旧输入会被跳过。

`FrameInput.draws` 借用 `DrawItem {mesh, material, model}` 列表，当前 Demo 根据两个开关提交 0、1 或 2 项。
不同绘制项可共享 Mesh 或 Material，各自独立设置 model，共用相机。提交返回后可修改或销毁原始列表；
GPU 网格、材质及材质依赖的纹理仍由 Renderer 持有至关闭。空列表只清屏并显示本帧 UI。

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
依次取得两个 Shader；程序几何模式还取得所选纹理，模型模式则注册 ModelLoader。
这些路由由 Demo 独占管理。清理成员在 Loader 注册前构造，覆盖后续加载以及
外层构造失败；已有登记条目显式 acquire，仅释放本次实际取得的持有。

Demo 在创建窗口前加载资源，随后以 `Renderer(window, validationMode)` 初始化运行环境，
通过 `createTexture(*assets.texture)` 显式上传，并调用
`prepareSceneResources(*assets.vertex, *assets.fragment)` 准备共享 Shader 与帧/材质布局。
随后创建两份共享 Texture 的材质：立方体使用默认白色，平面使用线性 RGB `{0.5, 1.0, 0.5}`。
`makeSceneDraws` 将两份材质分别写入对应 DrawItem，变换、开关和 Reset scene 不重新创建材质。
创建两种网格后立即释放 CPU 资源并检查纹理已卸载，再进入绘制循环。
模型模式在共享 Shader 准备后调用 `Model::prepareModel(renderer, paths.model)`，由准备入口加载
ModelResource 与 PNG、上传唯一几何/纹理并创建材质；不创建程序立方体和平面。
准备完成后释放 CPU Shader 并移除 Loader；CPU 模型、像素和 Importer 均不被实例借用。
每帧将可见对象转换为共享同一 PreparedModel 的 ModelInstance，使用 `makeModelDraws` 展开后提交。
Renderer 保留重建 Pipeline 所需的 Shader 字数组副本，并持有 GPU Texture 与不可变 Material；
材质颜色在 sRGB 采样后按线性 RGB 相乘，默认白色保留原色与方向，平面减少红/蓝线性分量；输出保持不透明。
复制应用引用共享同一次上传和材质绑定，交换链重建不重新读取图片。

加载、Renderer 初始化或显式资源准备失败时，SceneAssets 释放已取得的 CPU 引用、卸载归零对象，
移除 Loader，最后销毁 FileIO。正常关闭时 Renderer 先于窗口销毁。

## 验证模式与错误输入

`examples/demo/validation_mode.h` 将 Debug Demo 配置为 `Optional`，Release 配置为
`Disabled`。Optional 缺少 Khronos Validation Layer 或 `VK_EXT_debug_utils` 时会告警并继续；
需要强制启用验证的 GPU 测试显式使用 `Required`。

必需 Shader、模型或依赖缺失、读取或解码失败时，应用记录启动错误并返回退出码 1。
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
- [ModelLoader](../runtime/resource/loader/model_loader.md)：静态 glTF/OBJ 支持范围与材质/路径边界。
- [Model](../runtime/model.md)：GPU 准备、共享实例与矩阵展开。
- [CMake 构建结构](../cmake.md)：构建层级与通用资源部署约定。
