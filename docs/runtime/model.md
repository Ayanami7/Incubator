# Model 模型准备与实例

`runtime/model/prepared_model.h` 与 `model_instance.h` 提供模型消费层，依赖 Resource 和 Render，归入 `IncubatorRuntime`。
Resource 交付 CPU 数据，Renderer 创建并持有 GPU 对象，模型层负责依赖准备及可共享的只读结果。

## 准备入口

```cpp
// The application registers MODEL/TEXTURE loaders and keeps their FileIO alive.
// Shared scene shaders are prepared explicitly once on this Renderer.
renderer.prepareSceneResources(vertexShader, fragmentShader);
auto model = Incubator::Model::prepareModel(renderer, "models/avocado/glTF/Avocado.gltf");
auto sharedModel = model; // Shares immutable metadata and the same GPU references.
```

`prepareModel(Render::Renderer&, std::string_view, PrepareOptions = {})` 同步返回
`std::shared_ptr<const PreparedModel>`。`PrepareOptions` 包含 `mipMode`、`sampler` 与追加的 `shading`，默认分别为
`Render::TextureMipMode::BaseLevel`、默认 `Render::SamplerDesc` 与 `Render::ShadingMode::Unlit`：单层、nearest/repeat/LOD=0、不受光。
旧的两参数调用保持该行为；显式选项作用于本次准备使用的全部纹理与材质，包括共享的白图。

```cpp
Incubator::Model::PrepareOptions options;
options.mipMode = Incubator::Render::TextureMipMode::FullChain;
options.sampler = {Incubator::Render::TextureFilter::Linear, Incubator::Render::TextureFilter::Linear,
                   Incubator::Render::MipFilter::Linear};
options.shading = Incubator::Render::ShadingMode::Lambert;
auto filtered = Incubator::Model::prepareModel(renderer, "models/avocado/glTF/Avocado.gltf", options);
```

显式设置 `options.shading = Render::ShadingMode::Lambert` 可选择最小方向光漫反射预览；
源 `ModelMaterial::unlit` 为 true 的材质始终使用 Unlit。不同模式仍共用生产 Pipeline，
同次准备的几何、纹理、白图及采样共享规则保持；选项创建时复制，之后修改不改变已有材质。
非法 mip、采样或着色选项在加载或 GPU 创建前抛 RENDER，空绘制模型也遵守此约定。该模式不导入源灯光，也不代表 glTF PBR 或完整 MTL 材质还原。

`ModelReference(path, options)` 拥有规范化的 UTF-8 虚拟路径和完整准备配置；构造时拒绝非法路径和选项，
值相等比较包含路径、mip、全部采样选项和着色模式。`PreparedModel::source()` 返回只读来源，保存调用时请求的配置；
源 Unlit 材质不改变该记录。来源不持有 CPU 资源，也不创建缓存或延长 GPU 资源生命周期。

上传与采样选择独立：完整链可仅采基础层，单层图片也可绑定 mip 采样而不补建层级。
同次准备内，相同规范化路径仍共享一次上传及其 mip 链；白图在 FullChain 下仍只有 1×1 一层。
返回后本次 CPU 持有释放，后续相机操作和窗口重建继续使用已有 GPU 图像、mip 与材质，
不重新读取源文件。绘制项顺序、节点变换和 bounds 与采样策略无关。
参数在创建时复制，之后修改 options 不影响已有准备结果。它是消费者的预览策略，
不导入源 glTF/OBJ sampler，也不向 ModelResource 或 Loader 引入 Render 类型。

Demo 的会话级 `ModelCache` 借用该会话的 Renderer，按 `ModelReference` 的规范化路径和完整选项复用
`PreparedModel`；不同选项独立准备。缓存由 Demo 回调持有，并在 Renderer 关闭前释放，不跨会话共享。

路径相对资产根，经现有 ResourceManager/Registry 规范化和查找；应用须提前注册 MODEL 和 TEXTURE 的 Loader。
函数不安装或替换 Loader，不准备 Shader，不修改全局 Manager 的路由。线程安全仍由调用方保证，
GPU 准备在 Renderer 所在线程完成，FileIO 至少覆盖 CPU 导入与依赖读取期。

准备依次完成：

1. 取得 ModelResource 的本次持有，复制节点和局部矩阵，建立被节点引用的部分、几何及材质映射。
2. 通过 Manager 加载所有被使用材质的唯一 PNG 依赖，确认依赖完整后开始 GPU 创建。
3. 每份被使用几何上传一次，每个被使用的源材质创建一次；不同材质引用相同规范化路径时共享 Texture。
4. 纯色/默认材质共享本次准备内的一张显式 1×1 RGBA 全白纹理，无此类材质时不创建白纹理。
5. 从源根按深度优先顺序缓存绘制项及模型内部累计矩阵，同时合并实际绘制内容的边界；返回完整结果，并释放本次 CPU 模型和图片持有。

未被任何节点使用的部分、几何、材质及其纹理不准备；源 ModelResource 已在构造时校验结构，
准备层不重复校验不可变图。顶点从 `Resource::MeshVertexData` 显式转换为 `Render::MeshVertex`，
逐分量复制 position、uv 和几何局部 normal，不提前应用节点矩阵；索引保持 uint32。
Renderer 在提交时由最终 DrawItem::model 计算法线逆转置，包含实例、源根和内部节点累计变换，源根只计算一次。
共享以源几何/材质索引及纹理规范化路径为准，不按内容合并不同的源描述。再次显式调用准备允许产生另一组 GPU 对象。

## 只读结果

| 接口 | 内容与索引 |
|---|---|
| `source()` | 拥有规范化虚拟路径和请求配置的只读 ModelReference |
| `meshes()` | 唯一已上传 Mesh 的紧凑数组 |
| `materials()` | 被使用源材质对应的不可变 Material 紧凑数组 |
| `textures()` | 唯一 PNG Texture 和可选共享白纹理的数组 |
| `parts()` | 自有 ModelPart 描述，geometry/material 分别索引结果的 meshes/materials |
| `nodes()` | 自有 ModelNode 元数据；保留名称、局部矩阵、子节点顺序，parts 改为结果内部索引 |
| `rootNode()` | 源根在 nodes 中的索引；保留源根及非单位局部矩阵 |
| `draws()` | 缓存的只读 DrawItem 数组；每项 model 是从源根到所属节点的局部矩阵乘积，尚未包含实例整体变换 |
| `bounds()` | `const std::optional<Math::Aabb>&`；实际绘制内容的模型局部整体边界，已含源根和内部节点矩阵，尚未包含实例整体变换 |

节点数组及子节点索引保持源模型的顺序。资源和部分按节点数组中的首次引用组织为紧凑数组，
结果内部索引不可当作原 CPU 资产索引或 ResourceId。手工消费部分时，关联方式为：

```cpp
const auto& part = model->parts()[partIndex];
Incubator::Render::DrawItem draw{
    model->meshes()[part.geometry], model->materials()[part.material], worldMatrix};
```

`worldMatrix` 应包含实例与从源根到该节点的变换。常规多实例绘制使用下述入口。

### 模型整体边界

准备时复用缓存绘制的同一次节点遍历，将 [ModelResource 几何局部边界](./resource/model_resource.md#几何局部边界)
按当前节点累计矩阵变换并合并；未被节点引用的几何不参与。同一几何被多个节点引用时，
每次引用的累计矩阵均参与计算，源根只应用一次。

无绘制部分时 `draws()` 为空，`bounds()` 为 `nullopt`。任一实际绘制部分使用非仿射矩阵，
或变换无法产生有限边界时，整体边界为 `nullopt`，已有绘制项及顺序照常保留，不返回部分合并结果。
这不增加 ModelResource 的输入拒绝条件；使用边界的调用方在边界不可用时应保留绘制。
零跨度边界是有效值，不代表空模型。

边界只保存端点副本，不保留 CPU 顶点、像素或 ModelResource 借用；CPU 资源释放后仍可读取。
多个消费者通过同一 PreparedModel 共享只读结果；实例 Transform 改变不会改写模型边界。
Scene 帧提取在实体展开前使用此边界进行视锥裁剪；直接 `makeModelDraws` 和 Renderer 提交路径
保持调用方给定的绘制内容，不自动执行裁剪。

## 实例与绘制展开

```cpp
using Incubator::Model::ModelInstance;
using Incubator::Model::makeModelDraws;

std::array instances{
    ModelInstance{model, glm::translate(glm::mat4(1), glm::vec3(-1, 0, 0))},
    ModelInstance{model, glm::translate(glm::mat4(1), glm::vec3(1, 0, 0))}};
auto draws = makeModelDraws(instances);
renderer.drawFrame({extent, camera, draws}, clearColor);
// drawFrame borrows draws only during this call; storage may now be reused or released.
```

`ModelInstance` 保存 `std::shared_ptr<const PreparedModel> model` 与默认单位矩阵的 `glm::mat4 transform`。
多个实例可引用同一模型或不同模型。修改一个实例的 transform 不影响其他实例或模型缓存；实例不复制 CPU
几何、像素或 GPU 对象，也不分配 Entity ID。

[Scene](./scene.md#scene-与实体) 实体通过可选 `shared_ptr<const PreparedModel>` 共享准备结果，
使用独立 EntityId 和唯一 Transform 值。绑定或删除实体仅改变逻辑持有；实体整体矩阵在消费时转换为
ModelInstance，不改变 PreparedModel 的节点或 Renderer 的 GPU 持有期。
`Scene::extractSceneFrame` 默认先按实体世界包围体进行视锥筛选，再将保留的可见实体转换为临时 ModelInstance，复用 makeModelDraws 并返回自有
SceneFrame；应用在提交时借用其 input()，用法与快照持有期见 [场景帧提取](./scene.md#场景帧提取)。

`makeModelDraws(std::span<const ModelInstance>)` 每次返回一份新的 `std::vector<Render::DrawItem>`：

- 外层顺序为输入实例顺序；每个模型从 rootNode 开始，先输出当前节点的全部部分，再按 children 的原顺序
  深度优先访问子树；部分顺序与 node.parts 一致。重复引用的部分在每个所属节点各生成一个 DrawItem。
- 准备时以非递归遍历计算 `parent * localTransform`，根矩阵只包含一次。逐帧展开仅计算
  `instance.transform * cachedDraw.model`，不重新遍历静态树、不读盘、解码、上传或创建材质。
- 空输入或所有模型均无可绘制部分时返回空列表；实例数量变化不保留上次结果。每个实例必须持有非空 model，
  空模型指针抛 CONFIG；合法但无可绘制部分的 PreparedModel 可以正常引用。
- 相机和投影不参与实例展开，UV 与投影 Y 不再次适配。应用继续从 beginUiFrame 取得实际 extent 并生成相机；
  空列表也可交给 Renderer 清屏和绘制 UI。
- 输出持有普通的 Mesh/Material 弱引用和矩阵副本，借用期遵循 [Render](./render.md)。返回后修改实例不影响
  已生成列表；drawFrame 或 drawSceneTarget 返回后可立即修改或销毁列表，在途 GPU 依赖由 Renderer 持有。
- 展开不逐帧检查 GPU 引用或重验模型结构。Renderer 关闭后，实例仍可保有元数据并生成列表，但其中 GPU
  引用已失效，再次提交由 Renderer 明确拒绝；实例不会延长设备寿命。

最终 FrameInput、相机和 DrawItem 列表仍由应用提交；模型消费层不引入另一套帧同步。

启用阴影时，Scene 使用同一展开入口另行生成投射物列表，保持实体矩阵与模型内部累计矩阵的组合顺序。
PreparedModel 不保存阴影开关、光源范围或另一份几何；主相机外及 Unlit 模型也可投影。
阴影深度绘制仅消费网格和模型矩阵，接收阴影只影响 Lambert 直接光，具体提交约定见 [Render](./render.md#阴影帧参数)。

## 持有期及失败

- 模型及图片通过 ResourceManager::hold 取得只可移动的 CpuResource，首次与已有登记的引用由门面配对。
  正常或异常退出均释放本次持有，不消耗其他持有者的计数，不清除登记。模型准备不直接访问 Registry。
- CPU 加载失败的 RENDER 异常保留模型上下文、依赖虚拟路径及门面返回的具体原因；补充上下文不重复记录同一错误。
- 准备结果仅包含自有元数据与 Render 的不透明 GPU 引用，不借用 ModelResource、TextureResource、ShaderResource
  或 Importer。CPU 载荷卸载、Loader 销毁之后仍可使用结果。
- 已声明 PNG 缺失或解码失败使整个准备失败；不创建 GPU 对象，不以白纹理替代缺失的贴图。
  Manager 保留其既有日志 + nullptr 门面，准备入口抛出含模型/依赖路径的 RENDER 错误，不重复记录日志。
- GPU 创建错误直接沿用 Renderer 的异常。结果仅在全部创建成功后返回；失败前创建的 GPU 对象仍由
  Renderer 持有至关闭，不提供即时回收或 GPU 事务回滚。
- Mesh、Texture、Material 引用不延长 Renderer/设备寿命；共享或释放 PreparedModel 也不改变此约定。
  Renderer 关闭后引用失效，提交旧引用由 Renderer 的既有边界拒绝。

RGB 使用源材质的线性 0–1 参数，白纹理采样后保持该颜色；PNG 沿用 sRGB 采样后乘线性 RGB，输出 alpha=1。
默认最近邻、repeat、LOD=0；显式 PrepareOptions 可改变过滤及 mip 策略。
UV 方向、opaque 与其他显示约定见 [ModelLoader](./resource/loader/model_loader.md)
和 [Render 材质](./render.md)。

完整应用用法见 [模型 Demo](../examples/demo.md)：DemoApp 在 FileIO 生命周期内注册 Model/Texture Loader，准备 Shader 并将模型用于 Scene 实体。prepareModel 本身不清除全局路由。

[Scene Runtime 示例](../examples/demo.md) 由 DemoApp 持有 FileIO/Loader 到关闭，
使用会话模型缓存复用已准备的模型；创建实体或加载场景遇到新来源时准备，普通帧不重新上传。
