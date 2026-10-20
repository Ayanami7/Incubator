# Model 模型准备

`runtime/model/prepared_model.h` 提供模型消费层，依赖 Resource 和 Render，归入 `IncubatorRuntime`。
Resource 交付 CPU 数据，Renderer 创建并持有 GPU 对象，模型层负责依赖准备及可共享的只读结果。

## 准备入口

```cpp
// The application registers MODEL/TEXTURE loaders and keeps their FileIO alive.
// Shared scene shaders are prepared explicitly once on this Renderer.
renderer.prepareSceneResources(vertexShader, fragmentShader);
auto model = Incubator::Model::prepareModel(renderer, "models/minimal_gltf/model.gltf");
auto sharedModel = model; // Shares immutable metadata and the same GPU references.
```

`prepareModel(Render::Renderer&, std::string_view)` 同步返回 `std::shared_ptr<const PreparedModel>`。
路径相对资产根，经现有 ResourceManager/Registry 规范化和查找；应用须提前注册 MODEL 和 TEXTURE 的 Loader。
函数不安装或替换 Loader，不准备 Shader，不修改全局 Manager 的路由。线程安全仍由调用方保证，
GPU 准备在 Renderer 所在线程完成，FileIO 至少覆盖 CPU 导入与依赖读取期。

准备依次完成：

1. 取得 ModelResource 的本次持有，复制节点和局部矩阵，建立被节点引用的部分、几何及材质映射。
2. 通过 Manager 加载所有被使用材质的唯一 PNG 依赖，确认依赖完整后开始 GPU 创建。
3. 每份被使用几何上传一次，每个被使用的源材质创建一次；不同材质引用相同规范化路径时共享 Texture。
4. 纯色/默认材质共享本次准备内的一张显式 1×1 RGBA 全白纹理，无此类材质时不创建白纹理。
5. 返回完整结果，并释放本次 CPU 模型和图片持有。

未被任何节点使用的部分、几何、材质及其纹理不准备；源 ModelResource 已在构造时校验结构，
准备层不重复校验不可变图。顶点从 `Resource::MeshVertexData` 显式转换为 `Render::MeshVertex`，索引保持 uint16。
共享以源几何/材质索引及纹理规范化路径为准，不按内容合并不同的源描述。再次显式调用准备允许产生另一组 GPU 对象。

## 只读结果

| 接口 | 内容与索引 |
|---|---|
| `meshes()` | 唯一已上传 Mesh 的紧凑数组 |
| `materials()` | 被使用源材质对应的不可变 Material 紧凑数组 |
| `textures()` | 唯一 PNG Texture 和可选共享白纹理的数组 |
| `parts()` | 自有 ModelPart 描述，geometry/material 分别索引结果的 meshes/materials |
| `nodes()` | 自有 ModelNode 元数据；保留名称、局部矩阵、子节点顺序，parts 改为结果内部索引 |
| `rootNode()` | 源根在 nodes 中的索引；保留源根及非单位局部矩阵 |

节点数组及子节点索引保持源模型的顺序。资源和部分按节点数组中的首次引用组织为紧凑数组，
结果内部索引不可当作原 CPU 资产索引或 ResourceId。当前入口交付资源和元数据；整体实例变换和生产绘制列表
由实例消费入口组织。手工消费部分时，关联方式为：

```cpp
const auto& part = model->parts()[partIndex];
Incubator::Render::DrawItem draw{
    model->meshes()[part.geometry], model->materials()[part.material], worldMatrix};
```

`worldMatrix` 应包含实例与从源根到该节点的变换。最终 FrameInput、相机和 DrawItem 列表仍由应用提交，
借用期遵循 [Render](./render.md)；模型准备本身不创建实例、Scene Entity 或另一套帧同步。

## 持有期及失败

- 首次登记取得的 refCount=1 属于本次准备；已有登记通过 acquire 取得本次持有，包括此前计数为零的条目。
  正常或异常退出均逐项 release，不消耗其他持有者的计数，不清除登记。
- 准备结果仅包含自有元数据与 Render 的不透明 GPU 引用，不借用 ModelResource、TextureResource、ShaderResource
  或 Importer。CPU 载荷卸载、Loader 销毁之后仍可使用结果。
- 已声明 PNG 缺失或解码失败使整个准备失败；不创建 GPU 对象，不以白纹理替代缺失的贴图。
  Manager 保留其既有日志 + nullptr 门面，准备入口抛出含模型/依赖路径的 RENDER 错误，不重复记录日志。
- GPU 创建错误直接沿用 Renderer 的异常。结果仅在全部创建成功后返回；失败前创建的 GPU 对象仍由
  Renderer 持有至关闭，不提供即时回收或 GPU 事务回滚。
- Mesh、Texture、Material 引用不延长 Renderer/设备寿命；共享或释放 PreparedModel 也不改变此约定。
  Renderer 关闭后引用失效，提交旧引用由 Renderer 的既有边界拒绝。

RGB 使用源材质的线性 0–1 参数，白纹理采样后保持该颜色；PNG 沿用 sRGB 采样后乘线性 RGB，输出 alpha=1。
最近邻、repeat、LOD=0、UV 方向及其他固定显示条件沿用 [ModelLoader](./resource/loader/model_loader.md)
和 [Render 材质](./render.md) 的约定。
