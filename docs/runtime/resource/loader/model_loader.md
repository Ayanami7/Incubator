# ModelLoader：静态 glTF 与 OBJ 导入

## 入口与持有期

```cpp
#include "runtime/resource/loader/model_loader.h"
#include "runtime/resource/loader/model_resource.h"

using namespace Incubator::Resource;

auto fileIO = Incubator::FileIO::createPlatformFileIO();
auto& manager = Incubator::Resource::ResourceManager::instance();
manager.registerLoader(ResourceType::MODEL, std::make_unique<ModelLoader>(*fileIO));
const auto id = manager.registerResource(ResourceType::MODEL, "models/minimal_gltf/model.gltf");
if (const auto* model = manager.get<ModelResource>(id)) {
    // Read owned CPU geometry, materials, parts and nodes during this holding period.
}
manager.release(id);
manager.reset();  // Removes all Loader routes; do this before destroying their borrowed FileIO.
```

ModelLoader 为同步 `final LoaderBase`，借用构造时注入的 FileIO。每次加载创建局部 Importer，
在返回前把数据复制到 [ModelResource](../model_resource.md) 并释放 Importer 及文件句柄。
模型载荷不依赖导入器、源文件缓冲或 Loader 的后续存活。ResourceManager 的登记、缓存和引用计数语义不变：
重复登记不增加持有，已有条目需按约定 acquire，release 到零卸载对象。

失败时 Loader 抛带模型、依赖或材质索引上下文的 IO 异常；Manager 统一记录一次并返回 nullptr，
不挂载半成品。直接调用 Loader 时由调用者处理异常。模型导入沿用当前 ResourceManager 的调用方同步约定，
不并发执行导入；内部临时使用 Assimp 的进程级诊断观察器，调用结束后恢复。

## 文件与路径

资源根为启动工作目录下的 `assets/`，虚拟路径不带该前缀。模型、bin、mtl 均经私有
Assimp IOSystem/IOStream 适配进入 FileIO/FileHandle；不使用第三方默认文件读取路径。
IOStream 累积短读，保留完整元素计数、seek/tell/size 语义；提前 EOF 或读取异常使本次导入失败。
适配器支持只读输入，句柄由流独占，Importer 拥有适配器。

- glTF 的 bin 和图片 URI 相对 glTF 文件目录解析，百分号编码只在 URI 部分解码一次。
- OBJ 的 mtl 相对 OBJ 文件目录解析，map_Kd 相对声明它的 MTL 目录解析；沿用 Assimp 的目录前缀转换。
- 允许在 assets 根内使用 `..`。拒绝越出资源根、盘符、UNC 和网络依赖，不访问根外文件。
- 中文、空格和 Emoji 路径经 UTF-8 工具转换进入原生 FileIO。
- 必需 bin/mtl 缺失、读取失败或请求了不存在的命名材质均使模型导入失败；正常 Exists 探测不记为加载失败。
- PNG 只保留规范化虚拟路径，不由 ModelLoader 读取、登记或持有。PNG 缺失/解码失败属于后续准备入口的错误。

## 支持范围

固定依赖为 [Assimp v6.0.3](https://github.com/assimp/assimp/releases/tag/v6.0.3)，版本、许可证、下载哈希、
构建选项与固定版本细节见 [依赖来源说明](../../../../3rdparty/assimp/SOURCE.md)。最低 CMake 为 3.22。
该标签的上游 CMake/API 仍自报 6.0.2，依赖身份以固定标签与源码哈希为准。

| 输入 | 行为 |
|---|---|
| 静态 `.gltf` 2.0 | 外部 bin/PNG，保留源根、节点局部矩阵、部分及共享 aiMesh 关系 |
| `.obj` + 外部 `.mtl`/PNG | 三角化，保留导入器提供的根/物体节点，读取 Kd/map_Kd |
| 几何 | position、UV0、三角形列表；源顶点数量须能由 Assimp 表示，源索引转换为内部 uint32，越界索引拒绝 |
| 无贴图材质 | 默认白色或显式 0–1 线性 RGB；无 UV 时补固定 `(0,0)` |
| 基色贴图 | 仅一张外部 PNG、UV0；贴图材质缺少 UV0、非 UV0、显式 UV 变换（包括恒等）或多重基色贴图均拒绝 |
| glTF 透明字段 | 仅缺省/OPAQUE；忽略基色 alpha，不用通用 opacity 反向拒绝 OPAQUE；MASK/BLEND/未知模式失败 |
| OBJ 透明字段 | 最终 opacity 必须有限且等于 1，拒绝 map_d；d 直接赋值，Tr 赋 `1-Tr`，固定版本按最后声明覆盖 |
| 其他特征 | 拒绝动画、骨骼、Morph、GLB/嵌入图像或缓冲、压缩几何/图片及未支持的必需扩展；仅支持无需额外实现的 KHR_materials_unlit 必需扩展 |

普通光照、法线、金属度和其他非基色材质参数不进入当前顶点与 Shader 链路，只作基色预览。
源 sampler、双面参数不改变既有 nearest/repeat/LOD=0、opaque、无剔除的固定显示条件，不承诺完整 PBR 外观。
RGB 超范围或非有限、非法结构等错误继续由转换及 ModelResource 构造边界拒绝，不钳制或补默认资产。

Assimp 会压缩未使用的 glTF 顶点，并可能丢弃坏面、三角形余数或错误 UV 流。
导入前检查源 POSITION 数量是否可由 Assimp 表示、层级和会丢失的材质声明；这些修补诊断导致失败，不发布修补后的部分模型。
OBJ 未声明 Kd 时，转换为项目约定的白色；显式 Kd=0.6 保留灰色。
MTL 源检查只识别 Kd 是否声明及不支持的 map_Kd 选项，不另行解析材质数值或实现 MTL 导入器。
选项比较与固定 Assimp 一致，不区分 ASCII 大小写；`-o/-s/-t/-type` 及其大小写变体均拒绝。

## 矩阵、UV 与共享

Assimp 矩阵字段 a/b/c/d 表示行，转换时逐元素写入 GLM 列；局部矩阵和源根均不烘焙进顶点。
右手、Y-up 和源数值单位保持不变，不自动居中或归一化。每个 aiMesh 对应一份 MeshData 和一个整网格部分，
节点只保存部分索引；多个源节点引用同一 aiMesh 时继续共享几何。

固定版本先把 glTF 的 V 翻到 Assimp 的左下约定。转换层对 glTF 和 OBJ 输出统一使用 `(u, 1-v)`，
恢复引擎 `(0,0)` 指向 PNG 左上；图片解码、上传和 Shader 不再翻转。投影 Y 适配不属于模型导入。

原创模型及单位、节点矩阵、二进制布局和 UV 基准见 [样本说明](../../../../assets/models/SOURCE.md)。
`demo_models` 将模型和 sidecar 部署到构建资产目录；本接口只交付 CPU 资产，GPU 依赖与资源由
Runtime 的 [模型准备入口](../../model.md) 处理，实例展开由模型消费层组织。
