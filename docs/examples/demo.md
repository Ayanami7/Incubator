# 引擎模型 Demo

`examples/demo` 是唯一的引擎展示程序，由示例内的 `DemoApp` 组织 Window、Renderer、FileIO、Loader 和 Scene。
展示静态模型导入、GPU 资源准备、混合模型实例编辑、单方向光硬阴影及场景保存/加载。

## 构建与启动

按 [开发工作流](../../WORKFLOW.md) 执行 Debug/all 构建后，从构建目录运行：

```powershell
Set-Location 'E:/Project/VScode/Incubator/build'
& './examples/demo/demo.exe'
```

工作目录必须是对应构建目录。目标依赖 `demo_shaders`、`demo_models`、`demo_scenes` 和 `demo_config`，
复用 `<build>/assets/` 中的 SPIR-V、Avocado glTF/bin/PNG、Utah Teapot OBJ 和自有地面 OBJ/MTL。
默认展示场景为 `assets/scenes/shadows.json`，原 `assets/scenes/mixed.json` 继续部署并可直接打开。
Visual Studio 的目标调试工作目录也设为 `${CMAKE_BINARY_DIR}`。
VS Code 唯一的 `Debug` 配置指向本示例，完成构建后按 F5 启动。

Demo 还显式依赖 `runtime_ui_fonts`，部署 Noto Sans SC Regular 和原始 SIL Open Font License。
UI 使用 18 基础字号及统一控件间距，字体与样式随窗口 DPI 更新；窗口移动到不同缩放比例的显示器时，
在下一 UI 帧前同步重建字体，不逐帧缩放旧样式。字体覆盖英文和常用简体中文路径，完整范围见
`assets/fonts/SOURCE.md`。Docking 工作区在单操作系统窗口内提供对象、场景和属性三个主要面板。
字体及样式缩放与启动配置的窗口宽高分别处理，具体换算见 [Render](../runtime/render.md)。

程序从 cwd 读取 `incubator.json`。仓库根目录中的同名文件是默认配置源文件，构建时增量复制到
`<build>/incubator.json`；需持续保留的默认配置修改应写入源文件后重新构建。

```json
{
  "window": {"title": "Incubator Engine Demo", "width": 1920, "height": 1080},
  "startupScene": "assets/scenes/shadows.json"
}
```

配置文件不存在或字段省略时使用上述默认值；未知字段忽略。已有文件的读取失败、JSON 格式错误或字段无效会停止启动。
宽高要求正的 32 位整数；标题和启动场景要求非空 UTF-8 字符串，不能包含 NUL。窗口其他选项及 Validation 策略继续由代码决定。
配置路径和场景路径都按 cwd 解析，配置文件所在目录不改变场景路径的基准，加载期间不切换 cwd。
配置由 Demo 消费层通过现有 FileIO 和私有 RapidJSON 依赖读取，不提供 Runtime 配置管理器或自动回写。

默认启动场景 `assets/scenes/shadows.json` 包含 Avocado、Teapot 和中性灰色接收地面。
地面位于 y=-0.85，长宽各 10 个世界单位；Avocado 位于 `(-1.2,-0.8488543,0)`、缩放 24，
Teapot 位于 `(1.35,-0.259375,0)`、缩放 0.075，两者落在地面上。
相机位于 `(4,2.5,5.5)` 并观察 `(0,-0.2,0)`，垂直 FOV 45°、近/远裁面 0.1/100。
原 mixed.json 保留无地面的两个模型和 `(0,0,6)` 相机，兼容原文件编辑流程。
启动时通过既有场景文件读取和重建接口恢复实体、Camera、Lighting 和背景，不再固定创建两个 Avocado。
启动场景缺失、内容无效或模型准备失败时关闭会话并返回 1。
模型保留源节点层级、基色纹理和单位；面板创建对象时使用上述展示缩放。
两种模型准备时均显式选择 `TextureMipMode::FullChain` 和线性 min/mag/mip 过滤，寻址保持 Repeat。
Avocado 的基色 PNG 在 GPU 上生成完整 mip 链；Teapot 的共享 1×1 白图仍只有一层。
这是 Demo 的预览策略，未导入源 sampler；`prepareModel(renderer, path)` 的默认单层最近邻行为保持。
左侧面板用于实体和场景操作，没有自动运动。

## 操作

| 控件 | 行为 |
|---|---|
| 实体列表 | 按创建顺序显示 EntityId，点击选中 |
| Objects：New model / Create object | 选择 Avocado 或 Teapot，在画面中央按该模型的创建预设添加共享实体，并选中 |
| File：Save / Save As / Open | Save 写当前文件；Open 和 Save As 输入目标并确认，成功后才更新当前文件；取消和失败保留原关联 |
| Visible | 显示/隐藏当前实体，保留其变换 |
| Position / Rotation / Scale | 独立编辑实体的真实 TRS；支持零、负值及非均匀缩放，旋转使用角度并转换为四元数 |
| Delete selected | 删除选中实体，旧 ID 失效，其他实体保持 |
| Objects → More → Clear scene | 清空实体及选中状态，保留 Camera、背景和 UI |
| 场景右键拖动 / 中键拖动 / 滚轮 | 轨道旋转 / 平移 / 缩放；UI 上开始的手势不操作相机，场景手势跨面板保持归属 |
| F / Focus selected | 显式聚焦选中实体的世界包围体，保留朝向、FOV 和实体变换；F 受 UI 键盘捕获约束 |
| Reset view | 恢复位置 `(0,0,6)`、默认朝向和投影的相机，不重建实体或改变选择 |
| Background | 修改背景颜色 |
| Shadows / Receiver depth bias | 开关阴影预览，调整接收偏移；仅本会话有效，不写入场景文件 |
| Exit | 请求会话退出，也可直接关闭窗口 |

启动时仅准备场景实际引用的模型；创建实体时按需准备所选模型。
相同规范化来源与完整准备选项在当前会话内复用；不同选项分别准备。
创建及场景重建共用 Runtime ModelManager；Demo 不再维护单独的模型缓存实现。
准备后 CPU 模型和 PNG 持有已释放，Loader/FileIO 继续存活，因此后续操作仍走原有资源服务。
场景及阴影 Shader 同样通过 ResourceManager::hold 取得只可移动的 CPU 持有，准备结束后自动释放，不直接配对 Registry 引用。
调整姿态和可见性复用现有 GPU 引用。

保存包含所有实体（包括隐藏和无模型实体）、模型来源与准备配置、Camera、Lighting 和背景。加载成功后替换场景、清除选择并同步相机控制器；失败时显示错误并保留原场景。保存覆盖文件，打开后的写入失败可能留下部分文件。

保存成功后当前文件关联更新为目标文件；待操作路径单独保存，编辑它不改变当前文件，取消或失败也不改变关联。
多个场景使用不同文件路径分别保存，一次只打开一个 Scene。相对保存路径按 cwd 解析；
运行目录中的场景文件是部署副本，保存它不会回写仓库资产。
需要长期维护的项目场景可显式保存到仓库 `assets/scenes/` 的绝对路径，并补充对应部署规则；临时内容可保存在构建目录。

两种正式模型显式使用 Lambert 漫反射预览，继续采用 FullChain 和线性 min/mag/mip；
源 Unlit 材质始终不受光。默认阴影场景的传播方向为 `(-0.6,-1,-0.4)`、
线性颜色 `(1,0.95,0.9)`、强度 1、环境系数 0.15；GPU 帧参数会归一化方向。
Scene settings 折叠区提供 Light yaw/pitch、Color (linear)、Intensity 和 Ambient。
角度产生非零方向，颜色控件显示 `[0,1]` 线性 RGB，强度范围为 `[0,4]`，环境系数为 `[0,1]`。
强度 0 只关闭直接光，环境系数也为 0 时普通 Lambert 材质为黑色；清屏与 UI 不受灯光影响。
清空实体保留当前灯光；打开场景文件读取其中的灯光。相机操作和聚焦不会改变灯光。
这些参数直接读写当前 Scene，修改进入本帧提取，不重新准备模型、纹理、mip 或材质。
轨道操作、聚焦和窗口重建复用现有 GPU 资源；打开文件按需准备尚未缓存的模型，没有实时采样模式切换。
本会话的模型准备结果由消费者保留到关闭，不是全局资产缓存。

## 实现与持有期

`main.cpp` 创建 FileIO、读取启动配置，将同一 FileIO 交给 `DemoApp`，并以启动场景路径创建主循环回调；
`DemoApp::run()` 无参入口使用默认场景路径，不自行读取配置。`model_scene.h` 保存模型展示和面板操作。
`ModelScene` 保存会话模型复用表、当前选择、Demo 轨道目标/手势和少量面板状态；实体唯一 Transform、可见性、Camera
及光照由 Scene 保存。角度、TRS 与灯光编辑值在本轮 UI 中临时取得，发生修改才通过 Scene 接口写回。
创建/删除/清空时不跨结构变更保存实体指针；从文件加载成功后清除旧选择。

DemoApp 向回调消费者提供基于单调 Clock 的 FrameTime；生产 Demo 不显示循环间隔，也没有固定步长调度或自动实体动画。
事件→更新→本次 UI 批次相机操作→UI→SceneFrame 提取→Renderer 提交由 DemoApp 组织，UI 修改参与当次提取。
提取默认启用实体级视锥裁剪：完全离屏的实体保留在 Scene 和实体列表中，但不展开绘制项；
移回当前相机视锥后重新显示，实体变换、Camera 和场景切换在下一次提取生效。
裁剪使用模型准备时保存的整体边界，不改变模型原有展示缩放、材质或共享 GPU 资源。
全部实体离屏时仍清屏和显示 UI；需要对照完整绘制列表的调用方可直接使用
`Scene::extractSceneFrame(scene, extent, false)`，示例主循环沿用默认开启路径。
最小化时仍处理事件/更新，跳过 UI、投影、提取及提交；恢复使用新的实际 extent。
应用不手工缓存 FrameInput 或第二份实体 model 矩阵，帧数据持有及借用规则见 [Scene](../runtime/scene.md)。

回调共享的 ModelScene 在 run 收尾时释放，随后 DemoApp 清理 Scene、独占 Loader 路由和源绑定，再销毁 Renderer、Window、AssetSource 和 FileIO。
删除实体、清空或切换 Scene 后，ModelManager 继续保留缓存；显式 collectUnused 才移除没有外部使用者的模型，
在途资源待 Fence 完成后回收。当前 Demo 未增加清理按钮；会话结束释放缓存，独立 Renderer 先关闭时模型晚析构也安全。
Shader、模型依赖或回调失败时 DemoApp 先清理并返回失败；模型 CPU 加载失败已由 ResourceManager 记录，
面板展示其具体原因，main 不重复记录 Model::LoadError，其余异常由原处理边界记录，不继续提交不完整帧。

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
正常退出和异常退出均先释放回调/场景及 CPU 持有，再清理路由和源绑定，最后销毁 Renderer、Window、AssetSource 和 FileIO。外部 CPU 持有必须在会话结束前释放。

## 轨道相机与聚焦

`orbit_camera.h` 仅属于示例层，Runtime Camera 继续是渲染位姿和投影真源。
旋转以世界 +Y 为上方，俯仰限制在两极以内；缩放保持正距离。鼠标增量不乘帧时间。
视口平移使用图像逻辑高度和逻辑位移；聚焦使用场景目标像素尺寸的宽高比，不重复应用 DPI 比例。

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
视口输入按当帧图像区域判断归属，工作区布局独立于场景内容。

## 停靠工作区与文件关联

默认窗口及缺省配置为 1920×1080，显式窗口尺寸仍有效。顶部 File 菜单提供打开、保存、另存为和退出，
View 菜单提供场景视口开关与 Restore default layout。Objects 位于左侧，Scene 居中，Properties 位于右侧；
对象列表使用左侧剩余高度，属性面板依次提供选中对象、观察操作和场景设置，空间不足时纵向滚动。
底部仅保留一行当前文件与操作结果，错误显示为红色，悬停可查看完整路径与消息。
面板可以停靠与重排，恢复默认布局不改变场景。
布局使用 ImGui 自带的 `incubator-layout.ini`，相对运行 cwd；该本地文件不属于场景 JSON 或 incubator.json。
生产启动恢复上次布局，测试在隔离目录保存自己的布局。已有布局不会被新版默认布局覆盖；需要应用新布局时使用 View → Restore default layout。

当前文件和待操作路径分别保存。Save 使用当前文件；没有关联时进入 Save As 确认流程。Open／Save As 成功才更新关联；
取消、失败和单独编辑目标路径均不改变当前文件。清空仍然编辑当前文件对应的内容。Open／Save As 使用独立路径窗口，失败反馈留在窗口中；恢复文件内容时通过 Open 重新打开当前文件。
默认启动目标是构建目录的部署副本，保存不会回写源资产；清理 build 会删除副本，重新部署资产也可能覆盖副本。相关说明可在 File → About saved files 和路径窗口查看。
写入仍为同步直接覆盖，打开后的失败可能留下部分文件。

### 视口相机输入

右键旋转和中键平移只从场景图像内的按下开始；进入区域时已经按住的按钮不会启动手势。
已开始的拖动跨面板继续，释放批次的末段位移仍会应用一次。滚轮仅在悬停图像时缩放，
即使处于跨区域拖动中也不把面板滚轮交给相机。F 要求 Scene 窗口具有键盘焦点且不处于文本输入。
点击图像可以获得视口焦点，停靠标题、菜单和属性面板不属于图像区域。

输入位置转为相对图像原点的逻辑坐标，平移使用图像逻辑高度；聚焦使用当前场景目标像素尺寸。
失焦、最小化、关闭或折叠视口、成功替换场景会取消手势，并等待队列排空、按钮和 F 释放后的中性批次，
恢复时不重放旧位移。失败加载不改变当前手势状态。原始输入仍只参与取消和中性恢复，操作仅消费 ImGui 批次。


## 阴影预览

生产回调默认开启阴影，设置由 DemoApp 持有，通过 `shadowSettings()` 编辑，并在 UI 返回后与当前 Scene 一起提取。
固定光源观察体中心为 `(0,0,0)`、横纵半尺寸为 `(6,6)`，近/远面距离为 0.1/20.1；图像固定 1024×1024。
范围随当前 Lighting 方向转动，不随主相机或视口自动拟合；悬停 Shadows 控件可查看固定范围。
阴影只作用于 Lambert 直接光，环境项、背景和 UI 保持原行为；范围外直接受光，边缘为未过滤的硬阴影。

Demo 接收偏移默认 0.003，控件范围 `[0,0.01]`，单位为零到一光源深度；Runtime 的 ShadowSettings 默认值仍为 0.001。
该展示预设用于抑制 Avocado/Teapot 上可见的自阴影斑点，不改变阴影深度绘制或叠加额外偏移机制。
设置不写入 SceneDescription、JSON v1 或 incubator.json；打开或重新加载场景保留当前预览开关与偏移，
光线方向取新 Scene 的原始 Lighting，不另存第二份方向。

首次存在有效独立视口且启用阴影时，DemoApp 经原有 ShaderResource 路由加载 `shaders/shadow.vert.spv`，
准备成功即释放 CPU Resource；失败报告 RENDER 并关闭会话。关闭或最小化视口不准备额外深度资源；
恢复、调整视口及场景文件切换复用已准备的 Shader、帧槽阴影图和模型缓存。
无布局回调的原直接绘制消费者默认不启用阴影，显式启用仍按 Renderer 约定被拒绝。
