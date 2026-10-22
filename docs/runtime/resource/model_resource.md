# ModelResource 与 CPU 几何

## 职责与入口

`MeshData` 位于 `runtime/resource/mesh_data.h`；`ModelResource` 及组合记录位于
`runtime/resource/loader/model_resource.h`。它们只拥有 CPU 数据，不读取文件，不包含 Assimp 类型、
Render 头文件或 GPU 引用。矩阵使用 Runtime 已有的 GLM。

```cpp
struct MeshVertexData {
    std::array<float, 3> position{};
    std::array<float, 2> uv{};
};
struct MeshData {
    std::vector<MeshVertexData> vertices;
    std::vector<uint32_t> indices;
};
struct ModelMaterial {
    std::optional<std::string> texturePath;
    std::array<float, 3> color{1, 1, 1};
};
struct ModelPart {
    uint32_t geometry = 0;
    uint32_t material = 0;
};
struct ModelNode {
    std::string name;
    glm::mat4 localTransform{1};
    std::vector<uint32_t> children;
    std::vector<uint32_t> parts;
};

ModelResource(ResourceId id, std::vector<MeshData> geometries,
              std::vector<ModelMaterial> materials, std::vector<ModelPart> parts,
              std::vector<ModelNode> nodes, uint32_t rootNode);
```

ModelResource 为 `final ResourceObject`，类型固定为 `ResourceType::MODEL`。构造参数按值接收，
可以复制源数组或移动所有权；对象通过 `geometries()`、`materials()`、`parts()`、`nodes()` 返回只读引用，
通过 `rootNode()` 返回根索引。初始状态沿用 `UNLOADED`，Manager 挂载后置为 `READY`。
源数组、临时字符串和导入器释放后，载荷仍独立有效。文件导入使用 [ModelLoader](./loader/model_loader.md)，模型 GPU 准备另行提供。

## 组合与共享

- `ModelPart::geometry/material` 分别索引本模型的几何和材质数组；每部分绘制一份完整几何，不表示索引区间。
- `ModelNode::parts` 索引部分数组，`children` 索引节点数组，顺序按输入保留。节点允许无部分，多个节点可以引用相同部分或几何。
- `rootNode` 显式指定源根，不要求为 0。所有节点必须组成以它为根的单父树；源根局部矩阵原样保存。
- 几何、部分、材质与节点索引仅在本资产内有效，不是 ResourceId、GPU 句柄或 Scene Entity ID。
- 合法的根节点可以没有几何、部分或材质，表示空模型；数组中已有的记录仍全部接受结构检查。

例如部分 `{geometry=0, material=0}` 与 `{geometry=0, material=1}` 复用同一份几何，
由两个节点分别引用即可表达不同材质；一个节点也可顺序引用多个部分。

## 数据与校验边界

ModelResource 构造时一次性校验，失败抛出 `Error::Exception(CONFIG)`，不发布半成品。
访问数据时不重复校验；未来 Loader 经 Manager 加载时沿用现有异常门面。

| 数据 | 接受条件 |
|---|---|
| 几何 | 非空顶点及三角形索引列表，索引数是 3 的倍数且不超过 uint32 绘制计数上限；每个 uint32 索引在本几何顶点范围内 |
| 顶点 | position 和 UV 均有限；UV 可以超出 0–1，沿用 repeat 语义，不钳制 |
| 材质 | 线性 RGB 有限且在 0–1，默认白色；不包含透明混合参数 |
| 纹理路径 | `nullopt` 表示纯色/默认材质；有值时必须是有效非空资源虚拟路径，经 `canonicalizeResourceKey` 规范化，不能以空串代替无贴图 |
| 部分 | 几何和材质索引有效 |
| 节点 | 局部矩阵所有元素有限，部分和子节点索引有效；无重复父边、环、断开节点或指回根的边 |

路径必须由导入方先相对声明文件目录解析为 assets 根内的虚拟路径；ModelResource 不解析 sidecar、
探测文件或加载 PNG。纯色仅保存颜色；未来准备层显式共享白纹理，Renderer 仍要求明确 Texture。
导入方将合法源索引转换为内部 uint32；源几何顶点数量仍须能由当前 Assimp 表示。
内部不按 16 位索引范围限制顶点数，也不拆分网格。

## 矩阵约定

保留现有右手、Y-up、GLM 列向量约定，局部矩阵原样保存，不居中、不缩放归一化、不烘焙顶点。
后续展开的模型矩阵为：

```text
instanceTransform * root.localTransform * ... * node.localTransform
```

源根包含一次，父矩阵在左；视图、投影和投影 Y 适配不进入模型资产。
零缩放、负缩放和重复几何引用合法，不因矩阵不可逆而改写输入。

手工组合示例：根为 `T(10,0,0)`，父为 `Rz(90°)`，子为 `T(2,0,0)*S(2,3,1)`。
按根→父→子组合后，子原点为 `(10,2,0)`，点 `(1,1,0)` 为 `(7,4,0)`；
再左乘实例 `T(0,5,0)` 后为 `(7,9,0)`。这里只约定数据关系，不提供每帧展开接口。

## 文件导入

ModelLoader 将静态 glTF 2.0（外部 bin/PNG）及 OBJ（外部 mtl/PNG）统一转换为上述数据。
只消费 position、UV0、uint32 三角形索引和基色；贴图材质要求 UV0，纯色几何可由导入方补固定 UV。
glTF baseColorFactor/Texture、OBJ Kd/map_Kd 映射为线性颜色和外部纹理路径，采样和输出沿用
[Render 材质约定](../render.md)。动画、骨骼、Morph、GLB/嵌入图像、透明混合及完整 PBR 不在首批范围。
具体文件格式和材质拒绝规则见 [ModelLoader](./loader/model_loader.md)；手工 CPU 构造不读取或检查实际文件。
CPU 资产及 PNG 依赖通过 Runtime 的 [模型准备入口](../model.md) 转换为共享 GPU 引用与独立元数据，
准备完成后可卸载 CPU 载荷；Resource 层本身不依赖 Render。
