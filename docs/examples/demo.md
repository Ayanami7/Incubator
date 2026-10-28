# 引擎模型 Demo

`examples/demo` 是唯一的引擎展示程序，由示例内的 `DemoApp` 组织 Window、Renderer、FileIO、Loader 和 Scene。
展示静态模型导入、GPU 资源准备、混合模型实例编辑及场景保存/加载。

## 构建与启动

按 [开发工作流](../../WORKFLOW.md) 执行 Debug/all 构建后，从构建目录运行：

```powershell
Set-Location 'E:/Project/VScode/Incubator/build'
& './examples/demo/demo.exe'
```

工作目录必须是对应构建目录。目标依赖 `demo_shaders`、`demo_models`、`demo_scenes` 和 `demo_config`，
复用 `<build>/assets/` 中的 SPIR-V、Avocado glTF/bin/PNG 和 Utah Teapot OBJ，以及展示场景 `assets/scenes/mixed.json`，不需要复制额外资产。
Visual Studio 的目标调试工作目录也设为 `${CMAKE_BINARY_DIR}`。
VS Code 唯一的 `Debug` 配置指向本示例，完成构建后按 F5 启动。

Demo 还显式依赖 `runtime_ui_fonts`，部署 Noto Sans SC Regular 和原始 SIL Open Font License。
UI 使用 18 基础字号及统一控件间距，字体与样式随窗口 DPI 更新；窗口移动到不同缩放比例的显示器时，
在下一 UI 帧前同步重建字体，不逐帧缩放旧样式。字体覆盖英文和常用简体中文路径，完整范围见
`assets/fonts/SOURCE.md`。Docking 能力已启用，当前界面尚未建立停靠工作区，仍保持单操作系统窗口。
字体及样式缩放与启动配置的窗口宽高分别处理，具体换算见 [Render](../runtime/render.md)。

程序从 cwd 读取 `incubator.json`。仓库根目录中的同名文件是默认配置源文件，构建时增量复制到
`<build>/incubator.json`；需持续保留的默认配置修改应写入源文件后重新构建。

```json
{
  "window": {"title": "Incubator Engine Demo", "width": 1280, "height": 720},
  "startupScene": "assets/scenes/mixed.json"
}
```

配置文件不存在或字段省略时使用上述默认值；未知字段忽略。已有文件的读取失败、JSON 格式错误或字段无效会停止启动。
宽高要求正的 32 位整数；标题和启动场景要求非空 UTF-8 字符串，不能包含 NUL。窗口其他选项及 Validation 策略继续由代码决定。
配置路径和场景路径都按 cwd 解析，配置文件所在目录不改变场景路径的基准，加载期间不切换 cwd。
配置由 Demo 消费层通过现有 FileIO 和私有 RapidJSON 依赖读取，不提供 Runtime 配置管理器或自动回写。

默认启动场景为 `assets/scenes/mixed.json`，包含一个 Avocado 和一个 Teapot。
Avocado 位于 `(-1.2, -0.78, 0)`、缩放为 24；Teapot 位于 `(1.65, 0, 0)`、缩放为 0.075。
相机位于 `(0, 0, 6)`、朝向 −Z，垂直 FOV 45°，近/远裁面 0.1/100。
启动时通过既有场景文件读取和重建接口恢复实体、Camera、Lighting 和背景，不再固定创建两个 Avocado。
启动场景缺失、内容无效或模型准备失败时关闭会话并返回 1。
模型保留源节点层级、基色纹理和单位；面板创建及重置仍使用上述展示缩放。
两种模型准备时均显式选择 `TextureMipMode::FullChain` 和线性 min/mag/mip 过滤，寻址保持 Repeat。
Avocado 的基色 PNG 在 GPU 上生成完整 mip 链；Teapot 的共享 1×1 白图仍只有一层。
这是 Demo 的预览策略，未导入源 sampler；`prepareModel(renderer, path)` 的默认单层最近邻行为保持。
左侧面板用于实体和场景操作，没有自动运动。

## 操作

| 控件 | 行为 |
|---|---|
| 实体列表 | 按创建顺序显示 EntityId，点击选中 |
| New model / Create entity | 选择 Avocado 或 Teapot，在画面中央按该模型的创建预设添加共享实体，并选中 |
| Scene file / Save / Load | 显示启动场景路径；编辑 UTF-8 路径保存或加载，成功后记住该路径；指定其他路径即可另存 |
| Visible | 显示/隐藏当前实体，保留其变换 |
| Position / Rotation / Scale | 独立编辑实体的真实 TRS；支持零、负值及非均匀缩放，旋转使用角度并转换为四元数 |
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

启动时仅准备场景实际引用的模型；创建实体、重置或切换展示场景时按需准备相应预设模型。
相同规范化来源与完整准备选项在当前会话内复用；不同选项分别准备。
准备后 CPU 模型和 PNG 持有已释放，Loader/FileIO 继续存活，因此后续操作仍走原有资源服务。
调整姿态和可见性复用现有 GPU 引用。

保存包含所有实体（包括隐藏和无模型实体）、模型来源与准备配置、Camera、Lighting 和背景。加载成功后替换场景、清除选择并同步相机控制器；失败时显示错误并保留原场景。保存覆盖文件，打开后的写入失败可能留下部分文件。

保存成功后面板路径更新为目标文件；程序调用的保存或加载失败不会改写面板路径。
多个场景使用不同文件路径分别保存，一次只打开一个 Scene。相对保存路径按 cwd 解析；
运行目录中的 `assets/scenes/mixed.json` 是部署副本，保存它不会回写仓库资产。
需要长期维护的项目场景可显式保存到仓库 `assets/scenes/` 的绝对路径，并补充对应部署规则；临时内容可保存在构建目录。

两种正式模型显式使用 Lambert 漫反射预览，继续采用 FullChain 和线性 min/mag/mip；
源 Unlit 材质始终不受光。重置或切换场景时初始化传播方向 `(-0.6,-0.8,-1)`、
线性颜色 `(1,0.95,0.9)`、强度 1、环境系数 0.15；GPU 帧参数会归一化方向。
Lighting 折叠区提供 Light yaw/pitch、Light color (linear)、Light intensity 和 Ambient。
角度产生非零方向，颜色控件显示 `[0,1]` 线性 RGB，强度范围为 `[0,4]`，环境系数为 `[0,1]`。
强度 0 只关闭直接光，环境系数也为 0 时普通 Lambert 材质为黑色；清屏与 UI 不受灯光影响。
清空实体保留当前灯光；重置或切换场景恢复上述展示预设。相机操作和聚焦不会改变灯光。
这些参数直接读写当前 Scene，修改进入本帧提取，不重新准备模型、纹理、mip 或材质。
轨道操作、聚焦、场景切换和窗口重建不会重新上传图像、生成 mip 或创建材质；没有实时采样模式切换。
本会话的模型准备结果由消费者保留到关闭，不是全局资产缓存。

## 实现与持有期

`main.cpp` 创建 FileIO、读取启动配置，将同一 FileIO 交给 `DemoApp`，并以启动场景路径创建主循环回调；
`DemoApp::run()` 无参入口使用默认场景路径，不自行读取配置。`model_scene.h` 保存模型展示和面板操作。
`ModelScene` 保存会话模型复用表、当前选择、Demo 轨道目标/手势和少量面板状态；实体唯一 Transform、可见性、Camera
及光照由 Scene 保存。角度、TRS 与灯光编辑值在本轮 UI 中临时取得，发生修改才通过 Scene 接口写回。
创建/删除/清空时不跨结构变更保存实体指针；重置或切换展示场景后选中新场景的实体；从文件加载成功后清除旧选择。

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
demo_app.cpp、startup_config.cpp、model_scene.h，链接 IncubatorRuntime 和 imgui，
并依赖 demo_shaders、demo_models、demo_scenes、demo_config。默认入口读取混合模型场景；面板允许混合创建模型并读取场景文件。

DemoApp 属于 examples，不编译进 IncubatorRuntime；它负责本程序的初始化、主循环和清理。
带回调的 run 重载仅供集成测试驱动相同生命周期。Runtime 保留 Resource、Render、Model、Scene 等能力。
Demo 消费层测试位于 `test/examples/demo/demo_test.cpp`，目标为 `demo_test`，启动 cwd 为构建目录；
需要修改输入的测试夹具在独立工作副本中运行并恢复 cwd。纯 CPU 轨道相机测试位于同目录的
`orbit_camera_test.cpp`。TestMate 和 CTest 的执行约定见 [开发工作流](../../WORKFLOW.md#6-运行测试)。
`startup_config_test` 验证不依赖窗口或 GPU 的配置解析与文件读取行为。
展示冒烟、场景命令与输入接入分别验证，精确光照、采样和裁剪由 Runtime 专项覆盖。
合成输入按 ImGui 已消费的批次安排；焦点丢失会清空鼠标位置，恢复后先提供有效坐标，再按下并移动，
不能按固定 update 轮数假定位移已经被消费。
原程序几何和精确纹理辅助代码位于 test/support/render_demo，供渲染测试使用。

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

## 独立场景视口

生产 Demo 的 Scene 窗口显示独立场景图像。每轮先建立视口布局，再处理输入和编辑，最后使用当前
Camera 与实体状态提取场景并合成 UI。视口关闭、折叠或没有正区域时只提交其他 UI，不生成无效投影。
回调消费者可使用新增的 `Callbacks::layout` 与 `DemoApp::sceneViewport`；未提供布局回调的消费者继续直接绘制。

目标尺寸为内容逻辑尺寸乘以各轴 framebuffer/窗口比例，统一向下取整，正尺寸至少为一个像素；
场景分配、Camera 投影与裁剪使用相同结果。Windows content scale 不再额外参与此换算。
视口区域的精确输入归属和完整停靠工作区由后续任务接入。
