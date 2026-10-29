# Scene：实体、Transform 与 Camera

[Scene Runtime 示例](../examples/demo.md) 展示共享模型实体的创建、独立 TRS、可见性、删除、清空，
以及 DemoApp 内的 Scene/Camera 替换；所有实体变换直接保存在 Runtime Transform 中。

`runtime/scene/transform.h` 和 `camera.h` 提供自有值类型，归入现有 `IncubatorRuntime`。
二者使用 GLM，不依赖 Demo、ImGui 或输入控制；Camera 的输出类型复用 Render::CameraMatrices。
`runtime/scene/scene.h` 和 `entity.h` 提供平铺实体管理及逻辑身份；`scene_frame.h` 提供拥有存储的场景帧提取。

## Scene 与实体

```cpp
Incubator::Scene::Scene scene;
scene.setCamera(Incubator::Scene::Camera({0, 0, 3}));
auto model = Incubator::Model::prepareModel(renderer, "models/avocado/glTF/Avocado.gltf");
const auto first = scene.createEntity(model, Incubator::Scene::Transform({-1, 0, 0}));
const auto second = scene.createEntity(model, Incubator::Scene::Transform({1, 0, 0}));
scene.setVisible(first, false);
scene.destroyEntity(first);
// second still owns the same immutable PreparedModel.
```

Scene 不可复制或移动，使用一个 vector 保存实体，属于单线程 Runtime；当前不提供 ECS 或实体父子树。
场景内容捕获、JSON 文件读写和重建由同模块的独立接口提供，见下文。
Entity 保存 `id`、`transform`、`visible` 和可空的 `shared_ptr<const PreparedModel> model`。
Transform 是实体整体变换的唯一真源，实体不另存 ModelInstance 或可修改的整体矩阵。
模型内部节点仍属于 PreparedModel，不按节点创建实体。

| 接口 | 行为 |
|---|---|
| createEntity(model={}, transform=Transform{}, visible=true) | 返回新 EntityId；无模型也可创建合法逻辑实体 |
| find(EntityId) const | 返回只读 Entity 指针；无效、已删除或其他 Scene 的 ID 返回 nullptr |
| setTransform/setVisible/setModel | 通过 ID 修改指定实体；未命中返回 false，其他实体不变；setModel 可传空解除绑定 |
| destroyEntity(EntityId) | 删除指定实体，未命中返回 false；其他存活实体相对顺序保持 |
| clear() | 清除全部实体和它们的模型持有，保留当前 Camera 与光照 |
| entities() const | 返回 span<const Entity>，按存活实体的创建顺序遍历；新实体追加到尾部 |
| camera() const / setCamera(Camera) | 只读访问或整体替换 Scene 自有 Camera；默认 Camera 有效，设置时复制其值 |
| lighting() const / setLighting(Render::Lighting) | 只读借用或整体替换 Scene 自有光照；非法设置抛 CONFIG，保留原值 |

`Render::Lighting` 是不含 Vulkan 类型的帧级值：`direction` 为世界空间光线传播方向，
有限且非零，允许非单位输入；`color` 为有限的 `[0,1]` 线性 RGB，`intensity` 为有限非负数，
`ambient` 为有限的 `[0,1]` 标量。默认分别为 `(0,0,-1)`、白色、1、0.1；强度 0 关闭直接光。
Scene 保存输入方向原值，构造光源矩阵及 GPU 帧参数时只归一化计算副本。新 Scene 使用默认光照，替换场景时由调用方
显式设置展示值；clear 不重置光照。不创建灯光实体，也不在材质中保存另一份光照。

模型 Demo 的灯光控件直接编辑当前 Scene，UI 返回后主循环才提取 SceneFrame；
打开场景成功后使用文件中的光照，普通相机操作、聚焦与实体编辑保留当前光照；阴影预览设置由 DemoApp 独立持有，不写入 Scene。

EntityId 是独立的 uint64 包装，默认 0 无效，只有 Scene 分配非零标识；value() 可用于诊断、operator== 用于比较。
所有 Scene 共用进程内单调序号，删除、clear 或 Scene 销毁后不复用旧值，其他 Scene 的 ID 不会命中本 Scene。
序号耗尽时创建抛 CONFIG，不回绕分配旧值；不提供跨进程身份或从资源/模型索引构造实体 ID 的接口。
valid() 只表示曾分配过非零值，实体是否仍存活须由所属 Scene 的 find 判断。

查询、遍历结果仅在当前访问期借用。createEntity/destroyEntity/clear 后重新查询全部实体指针、引用与 span；
不在遍历期间改变容器结构，持久引用保存 EntityId。transform、可见性、模型和相机修改应在更新/UI 阶段进行。
Scene 只暴露只读实体视图，状态修改通过其 ID 接口，输入 Transform/Camera 沿用自身的校验契约。

多个实体直接共享同一 PreparedModel，不复制几何、像素或 GPU 对象。模型绑定/实体操作不加载文件、不准备资源；
删除、解除模型或 clear 只结束对应逻辑持有，其他实体不受影响。即使 PreparedModel 的最后一个逻辑持有结束，
GPU 仍由 Renderer 持有到关闭；这与 [Model 持有期](./model.md#持有期及失败) 一致。

## 场景内容快照

`runtime/scene/scene_description.h` 提供 `SceneDescription` 和 `captureScene(scene, clearColor)`。
捕获按创建顺序复制全部实体，包括隐藏、离屏、无模型及无绘制内容的实体，保存可空模型来源、真实 Transform 和可见性；
同时复制 Camera、原始 Lighting 和调用方传入的背景。快照不含 EntityId、派生矩阵、GPU 引用或交互状态，
之后修改或销毁 Scene 不会改变它。捕获不读取文件或准备 GPU 资源。

背景 RGB 为 sRGB 编码、alpha 为线性值，各分量必须有限且位于 `[0,1]`；无效输入抛 CONFIG。
默认描述使用默认 Camera、Lighting 和背景 `{0.12, 0.16, 0.22, 1}`。`SceneDescription::valid()`
检查可直接修改的光照和背景；Transform、Camera 和 ModelReference 继续由各自类型保证合法性。

## 场景 JSON 文件

`runtime/scene/scene_file.h` 提供 `encodeScene`、`decodeScene` 以及显式接收 FileIO 的
`readScene` / `writeScene`。文本为 UTF-8、无 BOM 的 JSON v1：

```json
{
  "version": 1,
  "entities": [{
    "model": {"path": "models/example.gltf", "prepare": {
      "mipMode": "fullChain", "sampler": {"minFilter": "linear", "magFilter": "linear", "mipFilter": "linear"},
      "shading": "lambert"
    }},
    "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
    "visible": true
  }],
  "camera": {"position": [0, 0, 6], "rotation": [0, 0, 0, 1],
    "perspective": {"verticalFov": 0.7853982, "nearPlane": 0.1, "farPlane": 100}},
  "lighting": {"direction": [0, 0, -1], "color": [1, 1, 1], "intensity": 1, "ambient": 0.1},
  "clearColor": [0.12, 0.16, 0.22, 1]
}
```

字段均必需，`model` 可为 `null`；四元数顺序为 xyzw，FOV 使用弧度。枚举区分大小写：
`mipMode` 为 `baseLevel/fullChain`，min/mag 过滤为 `nearest/linear`，mip 过滤为
`baseLevel/nearest/linear`，`shading` 为 `unlit/lambert`。未知字段忽略；错误格式、非法值及其他版本抛 IO，
错误含字段上下文。内存描述无效时编码抛 CONFIG。编解码不加载模型或创建 GPU 资源。

文件接口接收 UTF-8 原生路径；模型路径仍相对资产根，与场景文件所在目录无关。
保存先完整编码，再创建或截断目标并同步写入、flush；打开后发生 I/O 错误可能留下截断或部分文件，
调用方的内存场景不受影响。接口不自动创建目录，也不提供原子替换。

`runtime/scene/scene_rebuild.h` 的 `rebuildScene(description, resolver)` 先校验内容，按实体顺序通过
调用方解析器取得模型并构造新 Scene，返回新场景及背景；空模型不调用解析器。新实体分配新 ID，
真实 Transform、Camera 和 Lighting 原样恢复，不重复应用模型源根或展示缩放。解析失败或返回空模型时抛出异常，
不返回部分场景；调用方应在成功后替换当前场景。解析期间已创建的 GPU 对象仍由 Renderer 持有至关闭。

## 场景帧提取

```cpp
// 先更新 Scene，UI 编辑同样在提取之前写回。
if (const auto extent = renderer.beginUiFrame())
{
    // 构建 UI 并将编辑结果应用到场景。
    auto frame = Incubator::Scene::extractSceneFrame(scene, *extent);
    renderer.imgui().endFrame();
    if (frame)
        renderer.drawFrame(frame->input(), clearColor, recordUi);
    // drawFrame 返回后可以修改或释放帧及绘制存储。
}
```

`extractSceneFrame(const Scene&, Render::FrameExtent, bool frustumCulling = true, const Render::ShadowSettings& shadows = {})` 返回 `std::optional<SceneFrame>`。
宽高任一为零时返回 nullopt，不计算投影或遍历实体；通常直接使用 beginUiFrame 给出的有效 extent。
提取期间不修改同一 Scene，不加载文件、解码、上传、创建材质或重新验证 GPU 引用。

SceneFrame 是自有值，保存 `extent`、`Render::CameraMatrices camera`、`vector<Render::DrawItem> draws`
及 `Render::Lighting lighting`、可空的 `Render::ShadowFrame shadow` 和自有 `shadowDraws` 列表：

- 读取当前 Camera，按实际 extent 生成一次最终相机矩阵，供视锥判断和返回结果共用；不再次翻转 Y。
- 复制当前光照值；后续修改 Scene 光照或替换 Scene 不影响已提取帧，复制/移动后的帧也独立持有光照。
- 按存活实体创建顺序遍历，跳过隐藏及无模型实体；默认以当前实体 Transform 变换模型整体边界，
  只排除世界 AABB 完全位于视锥外的实体，再组装保留实体的临时 ModelInstance，调用现有 makeModelDraws。
  每个模型沿用其缓存的节点/部分顺序；最终矩阵为 `entity.transform.matrix() * cachedDraw.model`，源根只包含一次。
- 每次返回新列表；空 Scene、全部隐藏、全部被裁剪、无模型或无可绘制部分时返回有效的空 draws，仍可清屏和显示 UI。
- 两个结果可并存，默认复制/移动同时处理各自的值和 vector。Scene 更新、clear、下一次提取及 Scene 销毁
  不回写旧结果；快照不持有 Scene、Entity 或 PreparedModel 指针，绘制项只保存 GPU 引用和矩阵值。

第四个参数默认关闭阴影，保持原有调用；启用时在主相机剔除之前从全部 visible 且有模型的实体收集投射物，
沿用 `makeModelDraws` 展开顺序及矩阵乘法。主相机外、光源范围外及 Unlit 几何仍进入投射列表，
隐藏和无模型实体不进入；不在 CPU 上另做光源裁剪。关闭时 shadow 为空，不展开第二份绘制列表。
有效尺寸下，无效阴影参数抛 CONFIG；零尺寸仍优先跳过。固定范围与坐标规则见 [阴影帧参数](render.md#阴影帧参数)。
阴影设置属于消费者预览状态，不进入 Scene、SceneDescription、场景 JSON v1 或启动配置。

关闭 `frustumCulling` 时恢复原有可见实体的完整展开语义，隐藏及无模型过滤始终保留。
裁剪仅发生在本提取入口；直接调用 `makeModelDraws` 或 Renderer 不自动裁剪。
模型整体边界不可用、世界变换无法产生有限仿射边界或视锥不可用时保留原绘制路径。
边界接触和相交的判断沿用下述 [视锥约定](#视锥与-aabb-判断)。
世界边界每次由实体当前 Transform 派生，不缓存、不改写 `Entity.visible`，不删除实体。
Transform、模型绑定、可见性、Camera 位姿/FOV、extent 或当前 Scene 改变，下一次提取立即生效；
移出后移回的实体重新出现，最终模型矩阵仍只包含一次源根。

`SceneFrame::input() const &` 在调用时构造 FrameInput，两份 span 分别指向当前结果的 draws 和 shadowDraws，
相机、光照和可空阴影参数按值复制；结果本身不保存 span。
只能从左值结果取得视图，直接对临时 SceneFrame 调用 input 被禁止。借用后不要移动/修改结果或重新分配任一绘制列表，
保持其存储活到 `drawFrame` 或 `drawSceneTarget` 返回；复制或移动后，从目标对象重新调用 input，不保留旧视图。
提交调用返回后即可释放或改写 CPU 列表，在途 GPU 持有仍由 Renderer 保护。
绘制项中的 GPU 引用为非拥有引用。若使用显式 GpuResources，提取至提交期间须保留其对应上层资产或资源组；
快照的自有 vector 和复制语义不会延长 GPU 对象寿命，不能在提取与提交之间回收所用组。组的完整持有约定见 [Render](./render.md#显式可释放的-gpu-资源组)。

启用阴影的快照须通过 `drawSceneTarget` 提交，并事先准备深度 Shader；`drawFrame` 拒绝非空 shadow。
关闭阴影时仍可使用原直接呈现入口，不需要准备深度 Shader。

若提交前窗口 extent 改变，Renderer 跳过旧输入，下一轮使用新 extent 重新提取。
Renderer 关闭后，Scene/快照仍可保留元数据，提取不会检查或修复过期引用；提交仍由现有 Renderer 边界抛 RENDER。
[DemoApp 示例](../examples/demo.md) 已提供事件→更新→UI→提取→提交的驱动，持有当前 Scene 并支持
在下一次提取前 setScene 替换。替换只改变逻辑场景及相机，不清除 Loader 或即时回收 GPU。
提取接口可由应用独立调用。

## Transform

```cpp
using Incubator::Scene::Transform;

Transform pose({1, 2, 3}, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)), {2, 3, 4});
pose.setPosition({2, 0, 0});
const glm::mat4 model = pose.matrix();
```

- 构造参数依次为 position、rotation、scale，默认分别为零、单位四元数、全一缩放。
- position 和 scale 必须有限；允许有限的零和负缩放。位置使用所属场景/模型的数值单位，缩放无量纲。
- rotation 使用 GLM 四元数（构造参数顺序 w、x、y、z）；构造和 setRotation 接受任意有限、非零长度输入，
  统一归一化后存储。归一化使用 double 中间值，避免有限 float 四元数的平方溢出或下溢。
- position()/rotation()/scale() 返回只读引用；修改分别使用 setPosition/setRotation/setScale。
  非法输入抛 Error::Exception(CONFIG)，设置失败时保留原值；复制的 Transform 独立持有数据。
- matrix() 使用列向量，结果为 `T * R * S`，即先缩放、再旋转、后平移，不再次归一化或校验已接受输入。

欧拉角、角度单位及控件属于消费者。Demo 将角度转为弧度，再用 GLM 四元数保持原 `Rz * Ry * Rx` 顺序。

## Camera

```cpp
using Incubator::Scene::Camera;

Camera camera({0, 0, 3}, glm::quat(1, 0, 0, 0), {glm::radians(45.0f), 0.1f, 50.0f});
camera.setPerspective({glm::radians(60.0f), 0.1f, 100.0f});
if (const auto extent = renderer.beginUiFrame())
{
    const auto matrices = camera.matrices(*extent);
    // Generate draws, then borrow the list only for drawFrame.
    renderer.imgui().endFrame();
    renderer.drawFrame({*extent, matrices, draws}, clearColor);
}
```

Camera 保存世界位置、单位旋转及 Perspective，不保存缩放。局部前方为 −Z，上方为 +Y；默认位置为原点、
旋转为单位四元数，垂直 FOV 为 45° 对应的弧度值，near=0.1、far=100。单位相机的 view 为单位矩阵。

| 接口 | 约定 |
|---|---|
| Camera(position, rotation, perspective) | 三项均有默认值；位置有限，旋转遵循与 Transform 相同的归一化规则 |
| position()/rotation()/perspective() | 只读访问；Camera 复制后独立持有数据 |
| setPosition/setRotation | 设置世界刚体位姿；旋转从相机局部坐标映射到世界坐标 |
| setPerspective(Perspective) | 整体设置 verticalFov（弧度）、nearPlane、farPlane；有限且 0<FOV<π、0<near<far |
| matrices(FrameExtent) | 返回 view 与最终 projection；宽高任一为零时抛 CONFIG，计算投影前即拒绝 |

无效位置、四元数或透视参数在构造/设置时抛 CONFIG；设置失败不改变已有状态。
view 为世界刚体位姿的逆，即 `R⁻¹ * T⁻¹`。projection 使用实际 extent 的宽高比、右手坐标和 0–1 深度，
near 映射为 0、far 映射为 1；最终 projection 的 Y 只在此处适配一次，调用方和 Render 不再次翻转。
没有可绘制 extent 时，上层跳过投影、提取与提交，不用零尺寸求宽高比。

Demo 的 target/yaw/pitch/distance 仍为轨道控件状态，转换为相机世界位置及 quaternion 后交给此类型。
Runtime Camera 不提供通用控制器、正交投影或多视口；示例层轨道操作和世界包围体聚焦见 [Demo](../examples/demo.md#轨道相机与聚焦)，原始输入由 Platform Input/Window 提供。

## 视锥与 AABB 判断

`runtime/scene/frustum.h` 提供纯 CPU 的 `Incubator::Scene::Frustum`，消费既有最终相机矩阵，
不复制 Camera 位姿或投影设置，不依赖窗口、Vulkan 或 ImGui 的运行环境。

```cpp
const auto matrices = scene.camera().matrices(extent);
const Incubator::Scene::Frustum frustum(matrices);
const bool possiblyVisible = frustum.intersects(worldBounds); // Math::Aabb in world space.
```

构造函数为 `explicit Frustum(const Render::CameraMatrices&) noexcept`；
`bool intersects(const Math::Aabb&) const noexcept` 表示可能相交，不保证精确可见。
调用方应先将模型边界变换到世界空间；模型或世界边界不可用时保留候选。
Scene 帧提取使用同一份最终矩阵构造视锥，并在实体展开之前进行判断。

视锥从最终 `projection * view` 提取六个朝内平面，不再次翻转 Y。
GLM 按列存储，以下 `r0`–`r3` 表示乘积矩阵的行：

| 平面 | 系数 |
|---|---|
| 左 / 右 | `r3 + r0` / `r3 - r0` |
| 两个 Y 侧面 | `r3 + r1` / `r3 - r1` |
| 近 / 远 | `r2` / `r3 - r2` |

这对应 `-w <= x,y <= w`、`0 <= z <= w`，近面使用零到一深度约定。
平面归一化后，用沿朝内法向最远的箱角点判断；只有该角点仍在平面外才排除整个箱。
跨面、贴面、相机位于箱内或大箱包含视锥均保留，不要求某个角点位于视锥内。
固定容差 `1e-5` 集中定义，单位为世界坐标单位；容差内的小幅越界保留。
平面系数和距离以 double 计算；任一平面退化或系数非有限时，整个视锥保留全部候选。
输入边界端点非有限时也保留候选；不抛异常、不输出逐帧日志，不提供容差调参接口。
