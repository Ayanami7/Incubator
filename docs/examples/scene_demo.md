# Scene Runtime 示例

`examples/scene_demo` 是使用 [Application::Session](../runtime/application.md) 的单窗口应用。
原有 [Demo](./demo.md) 继续展示程序几何及直接 Renderer/ModelInstance 路径。

## 构建与启动

按 [开发工作流](../../WORKFLOW.md) 执行 Debug/all 构建后，从构建目录运行：

```powershell
Set-Location 'E:/Project/VScode/Incubator/build'
& './examples/scene_demo/scene_demo.exe'
```

工作目录必须是对应构建目录。目标依赖既有 `demo_shaders`、`demo_textures` 和 `demo_models`，
复用 `<build>/assets/` 中的 SPIR-V、PNG、glTF/bin 和 OBJ/MTL，不需要复制额外资产。
Visual Studio 的目标调试工作目录也设为 `${CMAKE_BINARY_DIR}`。

初始显示两个共享 glTF 模型的实体，分别位于 `(-1.2, 0.3, 0)` 和 `(1.2, 0.3, 0)`，
缩放均为 0.65。相机位于 `(0, 0, 6)`、朝向 −Z，垂直 FOV 45°，近/远裁面 0.1/100。
模型保留源节点层级、材质颜色及方向纹理。左侧面板用于实体和场景操作，没有自动运动。

## 操作

| 控件 | 行为 |
|---|---|
| 实体列表 | 按创建顺序显示 EntityId，点击选中 |
| Create entity | 在原点创建当前模型的共享实体，单位 TRS，并选中 |
| Visible | 显示/隐藏当前实体，保留其变换 |
| Position / Rotation / Scale | 独立平移、旋转、缩放选中实体；旋转使用角度，转换为四元数写回 Runtime Transform |
| Delete selected | 删除选中实体，旧 ID 失效，其他实体保持 |
| Clear scene | 清空实体及选中状态，保留 Camera、背景和 UI |
| Reset scene | 用当前模型重建初始的两个实体和 Camera，旧 ID 失效 |
| Switch to OBJ / glTF | 替换为另一模型的两个实体及初始 Camera，不保留旧 Scene 内容 |
| Replace camera | 整体替换 Scene Camera，在距离 6 和 8 间切换 |
| Clear color | 修改背景颜色 |
| Exit | 请求会话退出，也可直接关闭窗口 |

启动时只准备 glTF。首次 update 后的下一轮 update 显式准备 OBJ，此前切换按钮禁用；
初次 CPU 模型和 PNG 持有已释放，Loader/FileIO 继续存活，因此后续准备仍走原有资源服务。
每种模型在此示例中准备一次；返回已准备的模型、创建实体、调整姿态和可见性都复用现有 GPU 引用。
这两份示例准备结果由消费者保留到关闭，不是全局资产缓存。

## 实现与持有期

`main.cpp` 创建 Session 并运行 `scene_demo.h` 提供的初始化、更新与 UI 回调。
`Demo` 只保存两份共享 PreparedModel、当前选择和少量面板状态；实体唯一 Transform、可见性及 Camera
由 Scene 保存。角度和 TRS 编辑值在本轮 UI 中临时取得，发生修改才通过 Scene 的 ID 接口写回。
创建/删除/清空时不跨结构变更保存实体指针；切换 Scene 后重新选中新 Scene 的实体。

Session 使用单调 Clock 每轮传入 FrameTime，面板显示本轮更新时间；没有固定步长调度或自动实体动画。
事件→更新→UI→SceneFrame 提取→Renderer 提交由 Session 组织，UI 修改参与当次提取。
最小化时仍处理事件/更新，跳过 UI、投影、提取及提交；恢复使用新的实际 extent。
应用不手工缓存 FrameInput 或第二份实体 model 矩阵，帧数据持有及借用规则见 [Scene](../runtime/scene.md)。

回调共享的 Demo 在 run 收尾时释放，随后 Session 清理 Scene、独占 Loader 路由、Renderer、Window 和 FileIO。
删除实体、清空或切换 Scene 不即时回收显存；GPU 资源由 Renderer 持有到关闭。
Shader、模型依赖或回调失败时 Session 先清理，再由 main 记录异常并返回 1，不继续提交不完整帧。

Debug 示例沿用 Optional Validation，Release 为 Disabled；需要强制验证的 GPU 测试使用 Required。
支持的模型、纹理、材质和同步边界分别见 [Model](../runtime/model.md)、[Resource](../runtime/resource/resource.md)
和 [Render](../runtime/render.md)。
