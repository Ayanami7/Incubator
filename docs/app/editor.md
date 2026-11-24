# Editor 场景编辑工具

`app/editor/` 是独立消费 IncubatorRuntime 的正式工具，构建目标为 `Editor`，输出程序为 `Editor.exe`。EditorApp 组织会话，EditorScene 承接工作区、实体编辑、资产浏览、文件和轨道交互。

## 构建与启动

按 [开发工作流](../../WORKFLOW.md) 执行 Debug/all 构建后，从构建目录运行：

```powershell
Set-Location 'E:/Project/VScode/Incubator/build'
& './app/editor/Editor.exe'
```

工作目录必须是对应构建目录。目标依赖 `incubator_shaders`、`incubator_models`、`incubator_scenes` 和 `incubator_config`，
复用 `<build>/assets/` 中的 SPIR-V、Avocado glTF/bin/PNG、Utah Teapot OBJ 和自有地面 OBJ/MTL。
默认展示场景为 `assets/scenes/shadows.json`，原 `assets/scenes/mixed.json` 继续部署并可直接打开。
Visual Studio 的目标调试工作目录也设为 `${CMAKE_BINARY_DIR}`。
VS Code 选择 `Debug` 调试项启动 Editor，cwd 为 build；`Demo` 调试项启动最小 Demo。

Editor 还显式依赖 `runtime_ui_fonts`，部署 Noto Sans SC Regular 和原始 SIL Open Font License。
UI 使用 18 基础字号及统一控件间距，字体与样式随窗口 DPI 更新；窗口移动到不同缩放比例的显示器时，
在下一 UI 帧前同步重建字体，不逐帧缩放旧样式。字体覆盖英文和常用简体中文路径，完整范围见
`assets/fonts/SOURCE.md`。Docking 工作区在单操作系统窗口内提供对象、资产、场景和属性面板。
字体及样式缩放与启动配置的窗口宽高分别处理，具体换算见 [Render](../runtime/render.md)。

程序从 cwd 读取 `incubator.json`。仓库根目录中的同名文件是默认配置源文件，构建时增量复制到
`<build>/incubator.json`；需持续保留的默认配置修改应写入源文件后重新构建。

```json
{
  "window": {"title": "Incubator Engine", "width": 1920, "height": 1080},
  "startupScene": "assets/scenes/shadows.json"
}
```

配置文件不存在或字段省略时使用上述默认值；未知字段忽略。已有文件的读取失败、JSON 格式错误或字段无效会停止启动。
宽高要求正的 32 位整数；标题和启动场景要求非空 UTF-8 字符串，不能包含 NUL。窗口其他选项及 Validation 策略继续由代码决定。
配置路径和场景路径都按 cwd 解析，配置文件所在目录不改变场景路径的基准，加载期间不切换 cwd。
配置由 Editor 消费层通过现有 FileIO 和私有 RapidJSON 依赖读取，不提供 Runtime 配置管理器或自动回写。

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
这是 Editor 的预览策略，未导入源 sampler；`prepareModel(renderer, path)` 的默认单层最近邻行为保持。
左侧面板用于实体和场景操作，没有自动运动。

## 操作

| 控件 | 行为 |
|---|---|
| 实体树列表与内联可见性 | 在 `Objects` 中按同级创建顺序以树形缩进展示名称与 `EntityId`（空名显示“未命名”，重名以 `(#id)` 区分）；有子级时显示折叠/展开按钮（折叠状态不改变当前选中），左侧 `##vis` 复选框按实体 ID 独立切换显示/隐藏而不抢占当前选中项，点击列表项选中；隐藏父级不隐藏子级 |
| 挂接 / 解除挂接 | 通过 `Objects → More...` 的 `Parent selected to... (keep local)` 与 `Unparent selected (keep local)` 挂接或解除挂接（保留局部变换），自动过滤自身与后代以防止循环；成功变更各记 1 步 Undo/Redo，无变化或非法操作不入栈 |
| 顶部快捷主工具栏 | 位于主菜单栏下方，提供 `Open...` / `Save`（固定 ID `###toolbar-save`） / `Save As...`、`Undo` / `Redo`、`Translate (W)` / `Rotate (E)` / `Scale (R)`、`Focus (F)` / `Reset View` 与 `Shadows` 快捷开关 |
| 选中包围盒与三轴 Gizmo 叠加显示 | 选中可见实体时，在 Scene 图像层先绘制选中实体自身世界 AABB 黄色辅助线（若有模型边界），再叠绘三轴 Transform Gizmo（锚点位于实体世界位置；无模型可见实体亦正常绘制）；X/Y/Z 轴默认红/绿/蓝配色，悬停或拖拽轴高亮加粗；不参与深度且不遮挡面板或弹窗，不持久化 |
| 三轴 Gizmo 拖拽与子级范围 | 根实体支持世界平移、世界旋转与局部缩放（单次拖拽折叠为 1 步 Undo，失焦自动回滚），驱动所有后代随动；选中子实体时保持当前 `GizmoMode` 值不变，首版仅在 `Translate` 且父级世界矩阵可逆时显示 Gizmo 并支持世界轴平移（通过父级世界逆矩阵反算局部位置），处于 `Rotate` / `Scale` 或父级奇异时隐藏 Gizmo、停用手势并在面板提示使用 `Properties` 局部编辑 |
| Name | 编辑选中实体名称，允许重名和空名；连续输入在确认/失焦后折叠为单步历史；名称不影响实体身份或模型缓存 |
| Objects：New model / Create object | 选择 Avocado 或 Teapot，在画面中央按该模型的创建预设添加共享实体，并选中 |
| Assets：面包屑导航 / Up / Refresh | 点击路径面包屑任意目录段直接跳转，或使用 `Up` / `Refresh` 返回上级及重新列出当前目录；列表按彩色类型徽章（`[DIR]` / `[MODEL]` / `[PNG]` / `[DEP]`）展示；不解析或热重载资源 |
| Assets：Add | 对 OBJ/glTF 候选文件按需准备，成功后加入场景并选中；其他文件没有加入入口 |
| Assets：Clear unused models | 显式移出没有外部模型持有的缓存项；待 Undo/Redo 恢复的历史引用与在途 GPU 资源保持有效，覆盖/淘汰或打开场景清空历史栈后可回收 |
| File：Save / Save As / Open | Save 写当前文件（无关联时转 Save As）；Open 和 Save As 弹窗内置可视化 `.json` 场景文件浏览器（自动扫描 `assets/scenes` 及当前关联目录，目录缺失静默返回空列表，单击填充路径、Open 下双击直接确认），同时支持手动输入路径；成功后清除脏标记并清空历史栈；取消和失败保留原关联与当前历史 |
| 未保存修改确认 | 文档处于脏状态（状态栏显示 `*`）时触发 `Open`、`Clear scene`、`Exit` 或窗口关闭按钮将弹出确认弹窗，支持 `Save`（无关联时衔接 `Save As` 成功再继续，取消则中止原操作）、`Discard` 与 `Cancel` |
| Edit：Undo / Redo / Duplicate / Delete | 撤销/重做最近编辑动作（上限 64 步，回退至已保存修订号自动清除脏标记）；支持窗口级 `Ctrl+Z` / `Ctrl+Y` / `Ctrl+Shift+Z`（无需视口聚焦，文本输入、弹窗及活动事务期间不误触发） |
| Visible | 显示/隐藏当前实体，保留其变换 |
| Shading | 为带模型的选中实体切换模型准备着色模式（`Unlit` / `Lambert` / `PBR`，控件 `##entity-shading`）；先结算 Gizmo 与面板事务，通过 `ModelManager` 解析并复用缓存，纳入单步 Undo/Redo（资产自带 `unlit` 材质仍按 Unlit 渲染）；准备失败保留原模型与历史 |
| Position / Rotation / Scale | 双列对齐布局（含 X/Y/Z 轴色标），独立编辑实体的真实 TRS 与右侧 `R##reset-pos/rot/scale` 分项重置按钮；支持零、负值及非均匀缩放，旋转使用角度并转换为四元数；连续拖动合并为单步 Undo |
| Duplicate | 复制并选中实体，保持变换、可见性和共享模型；名称追加“ 副本”，空名使用“未命名 副本” |
| Delete selected | 删除无直接子级的选中实体，撤销可恢复原 `EntityId` 与共享模型；若实体仍有子级则拒绝删除并在状态栏提示先解除挂接或移走子级 |
| Objects → More → Clear scene | 清空实体及选中状态，保留 Camera、背景和 UI（进入历史栈，撤销可完整恢复） |
| 场景右键拖动 / 中键拖动 / 滚轮 | 轨道旋转 / 平移 / 缩放；UI 上开始的手势不操作相机，场景手势跨面板保持归属 |
| F / Focus selected | 显式聚焦选中实体的世界包围体，保留朝向、FOV 和实体变换；F 受 UI 键盘捕获与 Gizmo 拖拽互斥约束 |
| Reset view | 恢复位置 `(0,0,6)`、默认朝向和投影的相机，不重建实体或改变选择 |
| View：FXAA | 控制独立视口 LDR FXAA 预览开关（控件 `##fxaa`，默认关闭）；仅本会话有效，切换在下一个有效视口帧生效，不产生脏状态或 Undo 记录 |
| Background | 修改背景颜色（调色盘或面板改动合并为单步 Undo） |
| Shadows / Receiver depth bias | 开关阴影预览，调整接收偏移；仅本会话有效，不写入场景文件，不置脏也不进入 Undo 栈 |
| Exit | 请求会话退出，也可直接关闭窗口（均受未保存修改拦截保护） |

启动时仅准备场景实际引用的模型；创建实体时按需准备所选模型，默认名称取模型文件名去掉扩展名。
相同规范化来源与完整准备选项在当前会话内复用；不同选项分别准备。
创建及场景重建共用 Runtime ModelManager；Editor 不再维护单独的模型缓存实现。
资产列表通过 ResourceManager 获取单层目录，显示根内虚拟路径；不创建递归索引，不进入目录链接。
PNG、bin/mtl 和未知文件分别显示类型，扩展名只代表候选类别，不证明内容可加载。
新模型路径不需要加入内置模型表；Assets 的 Add 使用单位 Transform，保持模型原有单位，必要时通过属性缩放或聚焦观察。
准备选项沿用 FullChain、线性 min/mag/mip 和 Lambert，并随虚拟来源一起保存，不写物理资产根或 GPU 引用。
加载失败时 Assets 和状态栏显示本次调用的模型路径及具体依赖或解析原因，保留原场景、选择和文件关联；修复文件后再次 Add 即可。
资源层已记录的加载失败不会由 Editor 重复记错；浏览访问失败单独显示，成功的空目录不显示错误。
Listed 只统计当前目录发现项，Cached 只统计 PreparedModel 缓存项，二者均不代表 CPU 加载数或 GPU 存活数。
删除、清空或打开其他场景后，模型仍可由缓存复用；Clear unused models 在编辑阶段、帧提取前显式移出未使用条目。
清理结果表示移出的缓存项数；此前已提交的主场景及阴影帧继续持有 GPU 对象，相关 Fence 完成并释放帧槽持有后才实际回收，不逐次等待整个设备空闲。
准备后 CPU 模型和 PNG 持有已释放，Loader/FileIO 继续存活，因此后续操作仍走原有资源服务。
场景及阴影 Shader 同样通过 ResourceManager::hold 取得只可移动的 CPU 持有，准备结束后自动释放，不直接配对 Registry 引用。
调整姿态和可见性复用现有 GPU 引用。

保存包含所有实体（包括隐藏和无模型实体）、模型来源与准备配置、Camera、Lighting、Display（`toneMapping` 与 `exposureEv`）和背景。加载成功后替换场景、清除选择并同步相机控制器；失败时显示错误并保留原场景。保存覆盖文件，打开后的写入失败可能留下部分文件。

保存成功后当前文件关联更新为目标文件；待操作路径单独保存，编辑它不改变当前文件，取消或失败也不改变关联。
多个场景使用不同文件路径分别保存，一次只打开一个 Scene。相对保存路径按 cwd 解析；
运行目录中的场景文件是部署副本，保存它不会回写仓库资产。
需要长期维护的项目场景可显式保存到仓库 `assets/scenes/` 的绝对路径，并补充对应部署规则；临时内容可保存在构建目录。

两种正式模型显式使用 Lambert 漫反射预览，继续采用 FullChain 和线性 min/mag/mip；
源 Unlit 材质始终不受光。默认阴影场景的传播方向为 `(-0.6,-1,-0.4)`、
线性颜色 `(1,0.95,0.9)`、强度 1、环境系数 0.15；GPU 帧参数会归一化方向。
Scene settings 折叠区提供 Light yaw/pitch、Color (linear)、Intensity、Ambient，以及 Tone mapping（`##display-tone-mapping`：`Off` / `Reinhard`）和 Exposure (EV)（`##display-exposure-ev`：`[-8, +8]` EV）。
模式切换立即记 1 步撤销历史；`Off` 模式下禁用曝光滑块并保留原 EV 值供重新开启 `Reinhard` 时恢复；曝光拖动实时更新画面并在结束时折叠为单步历史。
角度产生非零方向，颜色控件显示 `[0,1]` 线性 RGB，强度范围为 `[0,4]`，环境系数为 `[0,1]`。
强度 0 只关闭直接光，环境系数也为 0 时普通 Lambert 材质为黑色；清屏与 UI 不受灯光影响。
清空实体保留当前灯光；打开场景文件读取其中的灯光。相机操作和聚焦不会改变灯光。
这些参数直接读写当前 Scene，修改进入本帧提取，不重新准备模型、纹理、mip 或材质。
轨道操作、聚焦和窗口重建复用现有 GPU 资源；打开文件按需准备尚未缓存的模型，没有实时采样模式切换。
本会话的模型准备结果由 ModelManager 缓存，直至显式清理或会话关闭，不是全局资产缓存。

## 实现与持有期

`main.cpp` 创建 FileIO、读取启动配置，将同一 FileIO 交给 `EditorApp`，并以启动场景路径创建主循环回调；
`EditorApp::run()` 无参入口使用默认场景路径。`editor_controller.cpp` 统一承载文档工作流与未保存拦截、场景文件枚举、资产浏览、面板跨通道连续事务、视口 Gizmo/相机交互、快捷键判定与纯 C++ 视口叠加图元组装；`editor_scene.cpp` 与 `editor_theme.cpp` 仅负责暗灰工作室主题、ImGui 控件绘制、输入快照采集及向 `EditorController` 转发；`editor_operations.cpp` 实现编辑、脏状态跟踪、历史快照与保存加载；`gizmo.cpp` 实现视口三轴 Gizmo 投影、命中与射线求解。
`EditorController` 与 `EditorOperations`（组合 `EditorSelection`，连同 `gizmo.h` 与 `app/common/viewport_input.h` 头文件均不依赖 ImGui）维护会话模型复用表、唯一选中 `EntityId`、当前文件关联、基于操作/事务修订号的脏状态判定以及有界（64 步）轻量值快照栈（撤销通过 `Scene::restoreEntities` 还原既有 `EntityId` 与共享 `PreparedModel`，不触发重新读盘或 GPU 上传），不持有 Scene 或实体指针；`EditorScene` 仅作为薄 ImGui 视图与布局提交层，不复制第二份 Scene 或实体状态。实体唯一 Transform、可见性、Camera 及光照由 Scene 保存。角度、TRS 与灯光编辑值在本轮 UI 中临时取得，连续编辑在 `beginTransaction` / `commitTransaction` 内折叠为单步历史；未提交的面板事务在打开/清空/退出/关闭前会先结算进脏状态，以免绕过未保存拦截；拖拽/窗口失焦调用 `cancelTransaction` 回滚起点。
创建/删除/清空时不跨结构变更保存实体指针；从文件加载成功后清除旧选择并清空历史栈。操作规则、历史淘汰、纯 CPU 裁剪/投影与 Gizmo 求解由单元测试覆盖，面板事务、快捷键、`W`/`E`/`R` 模式切换、叠加绘制与视口交互由真实 ImGui 输入批次集成测试覆盖，独立进程保存与恢复（含 Gizmo 编辑与撤销重做后持久化）由 `editor_scene_restart` 验证，互不替代。

EditorApp 向回调消费者提供基于单调 Clock 的 FrameTime；生产 Editor 不显示循环间隔，也没有固定步长调度或自动实体动画。
事件→更新→本次 UI 批次相机操作→UI→SceneFrame 提取→Renderer 提交由 EditorApp 组织，UI 修改参与当次提取。
提取默认启用实体级视锥裁剪：完全离屏的实体保留在 Scene 和实体列表中，但不展开绘制项；
移回当前相机视锥后重新显示，实体变换、Camera 和场景切换在下一次提取生效。
裁剪使用模型准备时保存的整体边界，不改变模型原有展示缩放、材质或共享 GPU 资源。
全部实体离屏时仍清屏和显示 UI；需要对照完整绘制列表的调用方可直接使用
`Scene::extractSceneFrame(scene, extent, false)`，示例主循环沿用默认开启路径。
最小化时仍处理事件/更新，跳过 UI、投影、提取及提交；恢复使用新的实际 extent。
应用不手工缓存 FrameInput 或第二份实体 model 矩阵，帧数据持有及借用规则见 [Scene](../runtime/scene.md)。

回调共享的 EditorScene 在 run 收尾时释放，随后 EditorApp 清理 Scene、独占 Loader 路由和源绑定，再销毁 Renderer、Window、AssetSource 和 FileIO。
删除实体、清空或切换 Scene 后，ModelManager 继续保留缓存；显式 collectUnused 才移除没有外部使用者的模型，
在途资源待 Fence 完成后回收。Assets 面板调用显式清理；会话结束释放缓存，独立 Renderer 先关闭时模型晚析构也安全。
启动阶段 Shader、模型依赖或回调失败时 EditorApp 先清理并返回失败；编辑中的加入或打开失败由 EditorScene 显示并保留原场景，允许重试。
模型 CPU 加载失败已由 ResourceManager 记录，面板展示其具体原因，main 不重复记录 Model::LoadError，其余异常由原处理边界记录。

Debug 示例沿用 Optional Validation，Release 为 Disabled；需要强制验证的 GPU 测试使用 Required。
支持的模型、纹理、材质和同步边界分别见 [Model](../runtime/model.md)、[Resource](../runtime/resource/resource.md)
和 [Render](../runtime/render.md)。

## 构建与代码边界

按 WORKFLOW.md 执行 Debug/all，或单独构建 Editor。Editor 编译 main.cpp，链接 `IncubatorEditorSupport` 并依赖 incubator_assets。
Editor 的应用实现、面板、编辑操作和选中框由 `IncubatorEditorSupport` 编译，供 Editor 及其测试共用。
`EditorApp` 组合 `IncubatorAppCommon` 的 AppSession，由公共会话持有并驱动当前 Scene。

Editor 纯 CPU 操作、选中框投影与 Gizmo 解算测试位于 `test/app/editor/editor_cpu_test.cpp`（目标 `editor_cpu_test`，不依赖 Vulkan 或磁盘夹具）；GPU/窗口集成测试位于 `test/app/editor/editor_test.cpp`（目标为 `editor_test`，启动 cwd 为构建目录，需要修改输入的夹具在独立工作副本中按需复制模型并恢复 cwd）。纯 CPU 轨道相机测试位于 `test/app/common/orbit_camera_test.cpp`。TestMate 和 CTest 的执行约定见 [开发工作流](../../WORKFLOW.md#6-运行测试)。
`startup_config_test` 验证不依赖窗口或 GPU 的配置解析与文件读取行为。
展示冒烟、场景命令与输入接入分别验证，精确光照、采样和裁剪由 Runtime 专项覆盖。
合成输入按 ImGui 已消费的批次安排；焦点丢失会清空鼠标位置，恢复后先提供有效坐标，再按下并移动，
不能按固定 update 轮数假定位移已经被消费。
原程序几何和精确纹理辅助代码位于 test/support/render_demo，供渲染测试使用。

Editor 关闭前保持 Loader 和 FileIO 存活，允许初次准备后继续加载模型。
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

生产 Editor 的 Scene 窗口显示独立场景图像。每轮先建立视口布局，再处理输入和编辑，最后使用当前
Camera 与实体状态提取场景并合成 UI。视口关闭、折叠或没有正区域时只提交其他 UI，不生成无效投影。
回调消费者可使用新增的 `Callbacks::layout` 与 `EditorApp::sceneViewport`；未提供布局回调的消费者继续直接绘制。

目标尺寸为内容逻辑尺寸乘以各轴 framebuffer/窗口比例，统一向下取整，正尺寸至少为一个像素；
场景分配、Camera 投影与裁剪使用相同结果。Windows content scale 不再额外参与此换算。
视口输入按当帧图像区域判断归属，工作区布局独立于场景内容。

`Properties → View` 中的 `FXAA`（`##fxaa`）经 `EditorApp::fxaaEnabled()`（由 `AppSession` 持有）控制独立视口的固定质量 LDR FXAA，默认关闭。颜色处理顺序为线性 HDR 主场景 → 显示 Pass（曝光、色调映射与 sRGB 编码）→ 可选 FXAA → ImGui（界面、文字、选中框与 Gizmo）；开关在 UI 阶段切换，本帧布局、图像注册与提交继续共用 `SceneOutput` 内已记录的状态，改动从下一有效视口帧生效。首次在有效视口帧启用时，按需经 `ShaderResource` 路由加载并准备 `shaders/fxaa.frag.spv`（关闭状态及 Demo 不加载），随即释放 CPU 持有；切换不改变场景文件、脏状态或历史栈。

## 停靠工作区与文件关联

默认窗口及缺省配置为 1920×1080，显式窗口尺寸仍有效。顶部菜单同时提供 File（打开、保存、另存为、退出）、Edit（Undo / Redo / Duplicate / Delete）与 View（场景视口开关、Restore default layout）。顶部及状态栏在文档处于脏状态时显示 `*` 标记；触发打开、清空、退出或直接点击窗口关闭按钮会经 `AppSession` 的 `closeRequested` 拦截并确认保存或放弃，保证与输入窗口 `Enter` / `Escape` 及活动面板事务一致。Objects / Assets 作为左侧标签页共用高度，Scene 居中，Properties 位于右侧；对象列表使用左侧剩余高度，属性面板依次提供选中对象、观察操作和场景设置，空间不足时纵向滚动。底部仅保留一行当前文件与操作结果，错误显示为红色，悬停可查看完整路径与消息。面板可以停靠与重排，恢复默认布局不改变场景。
布局使用 ImGui 自带的 `incubator-editor-layout.ini`，相对运行 cwd；该本地文件不属于场景 JSON 或 incubator.json。生产启动恢复上次布局，测试在隔离目录保存自己的布局。已有布局不会被新版默认布局覆盖；需要应用新布局时使用 View → Restore default layout。

当前文件和待操作路径分别保存。Save 使用当前文件；没有关联时进入 Save As 确认流程。Open／Save As 成功才更新关联；取消、失败和单独编辑目标路径均不改变当前文件。清空仍然编辑当前文件对应的内容。Open／Save As 使用独立路径窗口，失败反馈留在窗口中；恢复文件内容时通过 Open 重新打开当前文件。默认启动目标是构建目录的部署副本，保存不会回写源资产；清理 build 会删除副本，重新部署资产也可能覆盖副本。相关说明可在 File → About saved files 和路径窗口查看。写入仍为同步直接覆盖，打开后的失败可能留下部分文件。

### 视口相机与 Gizmo 输入

右键旋转和中键平移只从场景图像内的按下开始；进入区域时已经按住的按钮不会启动手势。
已开始的拖动跨面板继续，释放批次的末段位移仍会应用一次。滚轮仅在悬停图像时缩放，
即使处于跨区域拖动中也不把面板滚轮交给相机。`F` 聚焦与 `W` / `E` / `R` 模式切换受文本输入、弹窗与拖拽互斥约束（窗口级 `Ctrl+Z` / `Ctrl+Y` / `Ctrl+Shift+Z` 可在无视口焦点或视口关闭时生效）。
点击图像可以获得视口焦点，停靠标题、菜单和属性面板不属于图像区域。

选中可见实体时，视口左键优先执行三轴 Gizmo 屏幕命中检测（8 逻辑像素容差，近距或 X/Y/Z 序优先）：命中轴后锁定当前模式与操作轴开始拖拽（平移与旋转按世界轴，缩放按实体局部轴，零缩放轴自动退化为加性步进，旋转按起始视线掠射角锁定平面或屏幕切向分支），拖拽途中屏蔽实体重选、空白取消、`F` 聚焦、模式切换与相机手势，支持移出视口继续拖拽；松开提交 1 步 Undo，失焦/最小化/切换选中自动取消并回滚至拖拽起点。未命中轴时，在图像内按下并释放左键仍按原有规则选择最近可见模型世界包围盒，点击空白取消选择；拖动、越出图像或同时操作相机时取消本次点击。

选中可见实体后，Scene 图像上会在同一裁剪域内先绘制世界 AABB 十二条黄色辅助边（若有模型边界），随后叠加绘制当帧视图深度恒定尺度的三轴 Gizmo（默认 X 红、Y 绿、Z 蓝，悬停或拖拽轴高亮加粗）；二者使用当帧编辑后的相机与实体变换，始终可见、不参与深度且不改变场景/阴影颜色路径、不遮盖其他面板或弹窗，也不写入场景文件。隐藏实体仍保留其选择状态，不显示或命中 Gizmo。

输入位置转为相对图像原点的逻辑坐标，平移使用图像逻辑高度；聚焦使用当前场景目标像素尺寸。
失焦、最小化、关闭或折叠视口、成功替换场景会取消手势与未提交的 Gizmo 拖拽并等待队列排空、鼠标按钮及 `F` / `W` / `E` / `R` / `Z` / `Y` 释放后的中性批次，
恢复时不重放旧位移或旧快捷键。失败加载不改变当前手势状态。原始输入仍只参与取消和中性恢复，操作仅消费 ImGui 批次。


## 阴影预览

生产回调默认开启阴影，设置由 EditorApp 持有，通过 `shadowSettings()` 编辑，并在 UI 返回后与当前 Scene 一起提取。
固定光源观察体中心为 `(0,0,0)`、横纵半尺寸为 `(6,6)`，近/远面距离为 0.1/20.1；图像固定 1024×1024。
范围随当前 Lighting 方向转动，不随主相机或视口自动拟合；悬停 Shadows 控件可查看固定范围。
阴影只作用于 Lambert 直接光，环境项、背景和 UI 保持原行为；范围外直接受光，边缘为未过滤的硬阴影。

Editor 接收偏移默认 0.003，控件范围 `[0,0.01]`，单位为零到一光源深度；Runtime 的 ShadowSettings 默认值仍为 0.001。
该展示预设用于抑制 Avocado/Teapot 上可见的自阴影斑点，不改变阴影深度绘制或叠加额外偏移机制。
设置不写入 SceneDescription、JSON v1 或 incubator.json；打开或重新加载场景保留当前预览开关与偏移，
光线方向取新 Scene 的原始 Lighting，不另存第二份方向。

首次存在有效独立视口且启用阴影时，EditorApp 经原有 ShaderResource 路由加载 `shaders/shadow.vert.spv`，
准备成功即释放 CPU Resource；失败报告 RENDER 并关闭会话。关闭或最小化视口不准备额外深度资源；
恢复、调整视口及场景文件切换复用已准备的 Shader、帧槽阴影图和模型缓存。
无布局回调的原直接绘制消费者默认不启用阴影，显式启用仍按 Renderer 约定被拒绝。
