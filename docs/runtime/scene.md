# Scene：实体、Transform 与 Camera

[Scene Runtime 示例](../examples/scene_demo.md) 展示共享模型实体的创建、独立 TRS、可见性、删除、清空，
以及 Session 内的 Scene/Camera 替换；所有实体变换直接保存在 Runtime Transform 中。

`runtime/scene/transform.h` 和 `camera.h` 提供自有值类型，归入现有 `IncubatorRuntime`。
二者使用 GLM，不依赖 Demo、ImGui 或输入控制；Camera 的输出类型复用 Render::CameraMatrices。
`runtime/scene/scene.h` 和 `entity.h` 提供平铺实体管理及逻辑身份；`scene_frame.h` 提供拥有存储的场景帧提取。

## Scene 与实体

```cpp
Incubator::Scene::Scene scene;
scene.setCamera(Incubator::Scene::Camera({0, 0, 3}));
auto model = Incubator::Model::prepareModel(renderer, "models/minimal_gltf/model.gltf");
const auto first = scene.createEntity(model, Incubator::Scene::Transform({-1, 0, 0}));
const auto second = scene.createEntity(model, Incubator::Scene::Transform({1, 0, 0}));
scene.setVisible(first, false);
scene.destroyEntity(first);
// second still owns the same immutable PreparedModel.
```

Scene 不可复制或移动，使用一个 vector 保存实体，属于单线程 Runtime；当前不提供 ECS、实体父子树或持久化。
Entity 保存 `id`、`transform`、`visible` 和可空的 `shared_ptr<const PreparedModel> model`。
Transform 是实体整体变换的唯一真源，实体不另存 ModelInstance 或可修改的整体矩阵。
模型内部节点仍属于 PreparedModel，不按节点创建实体。

| 接口 | 行为 |
|---|---|
| createEntity(model={}, transform=Transform{}, visible=true) | 返回新 EntityId；无模型也可创建合法逻辑实体 |
| find(EntityId) const | 返回只读 Entity 指针；无效、已删除或其他 Scene 的 ID 返回 nullptr |
| setTransform/setVisible/setModel | 通过 ID 修改指定实体；未命中返回 false，其他实体不变；setModel 可传空解除绑定 |
| destroyEntity(EntityId) | 删除指定实体，未命中返回 false；其他存活实体相对顺序保持 |
| clear() | 清除全部实体和它们的模型持有，保留当前 Camera |
| entities() const | 返回 span<const Entity>，按存活实体的创建顺序遍历；新实体追加到尾部 |
| camera() const / setCamera(Camera) | 只读访问或整体替换 Scene 自有 Camera；默认 Camera 有效，设置时复制其值 |

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

## 场景帧提取

```cpp
// Update Scene first; UI edits are also applied before extraction.
if (const auto extent = renderer.beginUiFrame())
{
    // Build UI and apply its changes to scene.
    auto frame = Incubator::Scene::extractSceneFrame(scene, *extent);
    renderer.imgui().endFrame();
    if (frame)
        renderer.drawFrame(frame->input(), clearColor, recordUi);
    // frame/draw storage may be modified or destroyed after drawFrame returns.
}
```

`extractSceneFrame(const Scene&, Render::FrameExtent)` 返回 `std::optional<SceneFrame>`。
宽高任一为零时返回 nullopt，不计算投影或遍历实体；通常直接使用 beginUiFrame 给出的有效 extent。
提取期间不修改同一 Scene，不加载文件、解码、上传、创建材质或重新验证 GPU 引用。

SceneFrame 是自有值，保存 `extent`、`Render::CameraMatrices camera` 和 `vector<Render::DrawItem> draws`：

- 读取当前 Camera，按实际 extent 生成最终相机矩阵；不再次翻转 Y。
- 按存活实体创建顺序组装临时 ModelInstance，跳过隐藏及无模型实体，调用现有 makeModelDraws。
  每个模型沿用其缓存的节点/部分顺序；最终矩阵为 `entity.transform.matrix() * cachedDraw.model`，源根只包含一次。
- 每次返回新列表；空 Scene、全部隐藏、无模型或无可绘制部分时返回有效的空 draws，仍可清屏和显示 UI。
- 两个结果可并存，默认复制/移动同时处理各自的值和 vector。Scene 更新、clear、下一次提取及 Scene 销毁
  不回写旧结果；快照不持有 Scene、Entity 或 PreparedModel 指针，绘制项只保存 GPU 引用和矩阵值。

`SceneFrame::input() const &` 在调用时构造 FrameInput，span 指向当前结果的 draws；结果本身不保存 span。
只能从左值结果取得视图，直接对临时 SceneFrame 调用 input 被禁止。借用后不要移动/修改结果或重新分配 draws，
保持其存储活到 drawFrame 返回；复制或移动后，从目标对象重新调用 input，不保留旧视图。
drawFrame 返回后即可释放或改写 CPU 列表，在途 GPU 持有仍由 Renderer 保护。

若提交前窗口 extent 改变，Renderer 跳过旧输入，下一轮使用新 extent 重新提取。
Renderer 关闭后，Scene/快照仍可保留元数据，提取不会检查或修复过期引用；提交仍由现有 Renderer 边界抛 RENDER。
[Application Session](./application.md) 已提供事件→更新→UI→提取→提交的驱动，持有当前 Scene 并支持
在下一次提取前 setScene 替换。替换只改变逻辑场景及相机，不清除 Loader 或即时回收 GPU。
提取接口仍可独立使用，当前 Demo 的直接 ModelInstance/Renderer 路径继续可用。

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
当前不提供相机控制器、输入状态、正交投影或多视口。
