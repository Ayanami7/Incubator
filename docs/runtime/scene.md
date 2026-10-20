# Scene 基础：Transform 与 Camera

`runtime/scene/transform.h` 和 `camera.h` 提供自有值类型，归入现有 `IncubatorRuntime`。
二者使用 GLM，不依赖 Demo、ImGui 或输入控制；Camera 的输出类型复用 Render::CameraMatrices。
当前实现范围为姿态和透视相机，实体管理与场景帧提取尚未接入。

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
