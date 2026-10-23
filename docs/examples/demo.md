# 引擎模型 Demo

`examples/demo` 是唯一的引擎展示程序，由示例内的 `DemoApp` 组织 Window、Renderer、FileIO、Loader 和 Scene。
展示静态模型导入、GPU 资源准备、共享实例、实体操作和场景切换。

## 构建与启动

按 [开发工作流](../../WORKFLOW.md) 执行 Debug/all 构建后，从构建目录运行：

```powershell
Set-Location 'E:/Project/VScode/Incubator/build'
& './examples/demo/demo.exe'
```

工作目录必须是对应构建目录。目标依赖 `demo_shaders` 和 `demo_models`，
复用 `<build>/assets/` 中的 SPIR-V、Avocado glTF/bin/PNG 和 Utah Teapot OBJ，不需要复制额外资产。
Visual Studio 的目标调试工作目录也设为 `${CMAKE_BINARY_DIR}`。
VS Code 唯一的 `Debug` 配置指向本示例，完成构建后按 F5 启动。

初始显示两个共享 Avocado glTF 模型的实体，分别位于 `(-1.2, -0.78, 0)` 和 `(1.2, -0.78, 0)`，
缩放均为 24，以适配模型原始单位。相机位于 `(0, 0, 6)`、朝向 −Z，垂直 FOV 45°，近/远裁面 0.1/100。
模型保留源节点层级、基色纹理和单位。切换到 Teapot OBJ 时使用 0.075 的展示缩放。
左侧面板用于实体和场景操作，没有自动运动。

## 操作

| 控件 | 行为 |
|---|---|
| 实体列表 | 按创建顺序显示 EntityId，点击选中 |
| Create entity | 在画面中央按当前模型的展示缩放创建共享实体，并选中 |
| Visible | 显示/隐藏当前实体，保留其变换 |
| Position / Rotation / Scale multiplier | 独立平移、旋转、缩放选中实体；缩放倍率乘以当前模型展示基准，旋转使用角度并转换为四元数写回 Runtime Transform |
| Delete selected | 删除选中实体，旧 ID 失效，其他实体保持 |
| Clear scene | 清空实体及选中状态，保留 Camera、背景和 UI |
| Reset scene | 用当前模型重建初始的两个实体和 Camera，旧 ID 失效 |
| Switch to OBJ / glTF | 替换为另一模型的两个实体及初始 Camera，不保留旧 Scene 内容 |
| 场景右键拖动 / 中键拖动 / 滚轮 | 轨道旋转 / 平移 / 缩放；UI 上开始的手势不操作相机，场景手势跨面板保持归属 |
| F / Focus selected | 显式聚焦选中实体的世界包围体，保留朝向、FOV 和实体变换；F 受 UI 键盘捕获约束 |
| Reset camera | 恢复当前展示场景初始相机，不重建实体或改变选择 |
| Replace camera | 整体替换 Scene Camera，在距离 6 和 8 间切换 |
| Clear color | 修改背景颜色 |
| Exit | 请求会话退出，也可直接关闭窗口 |

启动时只准备 Avocado glTF。首次 update 后的下一轮 update 显式准备 Utah Teapot OBJ，此前切换按钮禁用；
初次 CPU 模型和 PNG 持有已释放，Loader/FileIO 继续存活，因此后续准备仍走原有资源服务。
每种模型在此示例中准备一次；返回已准备的模型、创建实体、调整姿态和可见性都复用现有 GPU 引用。
这两份示例准备结果由消费者保留到关闭，不是全局资产缓存。

## 实现与持有期

`main.cpp` 创建 `DemoApp` 并调用 `run()`；`model_scene.h` 保存模型展示和面板操作。
`ModelScene` 保存两份共享 PreparedModel、当前选择、Demo 轨道目标/手势和少量面板状态；实体唯一 Transform、可见性及 Camera
由 Scene 保存。角度和 TRS 编辑值在本轮 UI 中临时取得，发生修改才通过 Scene 的 ID 接口写回。
创建/删除/清空时不跨结构变更保存实体指针；切换 Scene 后重新选中新 Scene 的实体。

DemoApp 使用单调 Clock 每轮传入 FrameTime，面板显示本轮更新时间；没有固定步长调度或自动实体动画。
事件→更新→本次 UI 批次相机操作→UI→SceneFrame 提取→Renderer 提交由 DemoApp 组织，UI 修改参与当次提取。
提取默认启用实体级视锥裁剪：完全离屏的实体保留在 Scene 和实体列表中，但不展开绘制项；
移回当前相机视锥后重新显示，实体变换、Camera 和场景切换在下一次提取生效。
裁剪使用模型准备时保存的整体边界，不改变模型原有展示缩放、材质或共享 GPU 资源。
全部实体离屏时仍清屏和显示 UI；需要对照完整绘制列表的调用方可直接使用
`Scene::extractSceneFrame(scene, extent, false)`，示例主循环沿用默认开启路径。
最小化时仍处理事件/更新，跳过 UI、投影、提取及提交；恢复使用新的实际 extent。
应用不手工缓存 FrameInput 或第二份实体 model 矩阵，帧数据持有及借用规则见 [Scene](../runtime/scene.md)。

回调共享的 ModelScene 在 run 收尾时释放，随后 DemoApp 清理 Scene、独占 Loader 路由、Renderer、Window 和 FileIO。
删除实体、清空或切换 Scene 不即时回收显存；GPU 资源由 Renderer 持有到关闭。
Shader、模型依赖或回调失败时 DemoApp 先清理，再由 main 记录异常并返回 1，不继续提交不完整帧。

Debug 示例沿用 Optional Validation，Release 为 Disabled；需要强制验证的 GPU 测试使用 Required。
支持的模型、纹理、材质和同步边界分别见 [Model](../runtime/model.md)、[Resource](../runtime/resource/resource.md)
和 [Render](../runtime/render.md)。

## 构建与代码边界

按 WORKFLOW.md 的 Debug/all 命令构建，唯一示例目标为 demo。该目标编译 main.cpp、
demo_app.cpp、model_scene.h，链接 IncubatorRuntime 和 imgui，并依赖 demo_shaders、demo_models。
默认入口固定展示 Avocado，模型在面板内切换，不再提供原纹理位置参数或 --model 命令行模式。

DemoApp 属于 examples，不编译进 IncubatorRuntime；它负责本程序的初始化、主循环和清理。
带回调的 run 重载仅供集成测试驱动相同生命周期。Runtime 保留 Resource、Render、Model、Scene 等能力。
原程序几何和精确纹理辅助代码位于 test/runtime/support/render_demo，供渲染测试使用。

Demo 关闭前保持 Loader 和 FileIO 存活，允许初次准备后继续加载模型。
正常退出和异常退出均先释放回调/场景持有，再清理路由、Renderer、Window 和 FileIO。

## 轨道相机与聚焦

`orbit_camera.h` 仅属于示例层，Runtime Camera 继续是渲染位姿和投影真源。
旋转以世界 +Y 为上方，俯仰限制在两极以内；缩放保持正距离。鼠标增量不乘帧时间。
平移使用窗口高度和窗口坐标位移；聚焦使用实际 framebuffer extent 的宽高比，不重复应用 DPI 比例。

手势仅由场景区域的首次按下开始，起始批次不应用按下前位移。已归属手势的末段位移即使与释放同批到达，也先应用再结束手势。
同批另一个手势的新按下可随后启动，但不重复使用该批位移。释放、失焦、无有效 extent、
相机重置和场景切换结束手势；恢复后的旧输入由 UiInputAdapter 的中性帧规则过滤。
没有操作时不写回 Camera；替换相机或场景时同步轨道状态，普通选择不自动聚焦。

聚焦按实体 Transform 变换 PreparedModel 整体边界，源根不再叠加；以当前朝向下八角点的横纵视野
需求计算距离，留 10% 平面边距，并按深度需要扩展近远面。点边界使用 0.01 的最小观察尺度。
无选中、已删除、无模型或边界不可用时不改变 Camera 或轨道状态。聚焦不导入、上传或修改资产/实体缩放。
