# ModelLoader：静态 glTF、GLB 与 OBJ 导入

## 入口与持有期

```cpp
#include "runtime/resource/loader/model_loader.h"
#include "runtime/resource/loader/model_resource.h"

using namespace Incubator::Resource;

auto fileIO = Incubator::FileIO::createPlatformFileIO();
AssetSource assets(*fileIO);
auto& manager = Incubator::Resource::ResourceManager::instance();
manager.bindAssetSource(&assets);
manager.registerLoader(ResourceType::MODEL, std::make_unique<ModelLoader>(assets));
{
    auto held = manager.hold<ModelResource>(ResourceType::MODEL, "models/avocado/glTF/Avocado.gltf");
    if (const auto* model = held.get<ModelResource>()) {
        // Read owned CPU geometry, materials, parts and nodes during this holding period.
    }
}
manager.reset();  // Removes all Loader routes; do this before unbinding AssetSource / destroying FileIO.
manager.bindAssetSource(nullptr);
```

ModelLoader 为同步 `final LoaderBase`，借用构造时注入的 AssetSource。每次加载创建局部 Importer，
在返回前把数据复制到 [ModelResource](../model_resource.md) 并释放 Importer 及文件句柄。
模型载荷不依赖导入器、源文件缓冲或 Loader 的后续存活。ResourceManager 的登记、缓存和引用计数语义通过 `CpuResource` 自动管理：
重复持有复用已挂载对象，最后持有结束时卸载对象。

失败时 Loader 抛带模型、依赖或材质索引上下文的 IO 异常；Manager 统一记录一次并返回带 `failure()` 的空 `CpuResource`，
不挂载半成品。直接调用 Loader 时由调用者处理异常。模型导入沿用当前 ResourceManager 的调用方同步约定，
不并发执行导入；内部临时使用 Assimp 的进程级诊断观察器，调用结束后恢复。

## 文件与路径

ModelLoader 借用 AssetSource；默认根为构造时 cwd/assets，也可显式指定，虚拟路径不带 assets 前缀。
模型、bin、mtl 均经私有 Assimp IOSystem/IOStream 适配，以根相对路径进入同一 AssetSource 和 FileIO/FileHandle；不使用第三方默认文件读取路径。
IOStream 累积短读，保留完整元素计数、seek/tell/size 语义；提前 EOF 或读取异常使本次导入失败。
适配器支持只读输入，句柄由流独占，Importer 拥有适配器。

- glTF 的 bin 和图片 URI 相对 glTF 文件目录解析，百分号编码只在 URI 部分解码一次。
- OBJ 的 mtl 相对 OBJ 文件目录解析，map_Kd 相对声明它的 MTL 目录解析；沿用 Assimp 的目录前缀转换。
- 允许在资产根内使用 `..`，拒绝绝对、盘符、UNC、网络和词法越根依赖。只保证词法边界，不对文件链接的最终物理位置提供隔离承诺。
- 中文、空格和 Emoji 路径经 UTF-8 工具转换进入原生 FileIO。
- 必需 bin/mtl 缺失、读取失败或请求了不存在的命名材质均使模型导入失败；正常 Exists 探测不记为加载失败。
- 外部 PNG/JPEG 只保留规范化虚拟路径。后续准备入口负责读取贴图并报告缺失或解码失败。

GLB 使用单文件输入，上限为 256 MiB。加载和外部导入共用容器校验，依次检查 2.0 文件头、JSON chunk 和可选 BIN chunk。
缓冲及图像通过文件内的 BIN 和 bufferView 引用，图像 MIME 类型限定为 `image/png` 和 `image/jpeg`。
模型加载阶段将材质引用的内嵌图像解码为模型自有 RGBA8 像素，同一张图像只解码一次。

## 支持范围

固定依赖为 [Assimp v6.0.3](https://github.com/assimp/assimp/releases/tag/v6.0.3)，版本、许可证、下载哈希、
构建选项与固定版本细节见 [依赖来源说明](../../../../3rdparty/assimp/SOURCE.md)。最低 CMake 为 3.22。
该标签的上游 CMake/API 仍自报 6.0.2，依赖身份以固定标签与源码哈希为准。

| 输入 | 行为 |
|---|---|
| 静态 `.gltf` 2.0 | 外部 bin 及 PNG/JPEG，保留源根、节点局部矩阵、部分及共享 aiMesh 关系 |
| 静态 `.glb` 2.0 | 单文件，支持 BIN 中的几何及 PNG/JPEG，沿用同样的静态模型与材质校验 |
| `.obj` + 外部 `.mtl` 及 PNG/JPEG | 三角化，保留导入器提供的根/物体节点，读取 Kd/map_Kd |
| 几何 | position、UV0、局部 normal、tangent[4]（xyz 切线方向 + w 手性 $\pm 1$）、三角形列表；源顶点数量须能由 Assimp 表示，源索引转换为内部 uint32，越界索引拒绝 |
| 无贴图材质 | 默认白色或显式 0–1 线性 RGB；无 UV 时补固定 `(0,0)` |
| 纹理槽（基色 / 法线 / 金属粗糙度） | 外部 PNG/JPEG 或 GLB 内嵌 PNG/JPEG，使用 UV0；glTF 解析 `normalTexture`（含 `scale`）、`metallicRoughnessTexture`、`metallicFactor`（省略取 `1.0`）与 `roughnessFactor`（省略取 `1.0`）。OBJ 默认 `normalScale = 1.0`、`metallic = 0.0`、`roughness = 0.45`。贴图材质缺少 UV0、非 UV0、显式 UV 变换（包括恒等）或同槽多重贴图均拒绝 |
| glTF 透明与双面字段 | 接收 OPAQUE（默认）、MASK、BLEND。保存基色 alpha（默认 1）、有限非负的 alphaCutoff（默认 0.5）及 doubleSided（默认 false） |
| glTF 自发光字段 | emissiveFactor 为三分量线性 RGB，位于 [0, 1]，默认全零。emissiveTexture 支持外部或 GLB 内嵌 PNG/JPEG，使用 UV0 |
| OBJ 透明字段 | 最终 opacity 必须有限且等于 1，拒绝 map_d；d 直接赋值，Tr 赋 `1-Tr`，固定版本按最后声明覆盖 |
| 其他特征 | 拒绝动画、骨骼、Morph、`data:` URI、压缩几何及未支持的必需扩展。必需扩展仅支持 `KHR_materials_unlit` |

若源网格自带 `TANGENT` 与副切线，按双精度对顶点法线执行 Gram-Schmidt 正交化并求解手性 $w = \pm 1$；若无原生切线但具有 `UV0` 与法线，基于最终 V 翻转后的 `UV0` 与三角形索引累加切线/副切线并正交化；无 `UV0` 或退化几何输出零切线 `{0, 0, 0, 0}`。
采样配置由模型准备选项决定。主场景消费 Alpha Mask、双面和自发光字段，Alpha Mask 同时作用于阴影投射。
glTF 必需或可选的 `KHR_materials_unlit` 通过 Assimp 的 `AI_MATKEY_SHADING_MODEL` / `aiShadingMode_Unlit`
保存到 `ModelMaterial::unlit`；普通 glTF 和 OBJ 默认 false，不扩展 MTL 光照模型映射。
模型准备入口默认 nearest/repeat/LOD=0，消费者可通过 [PrepareOptions](../../model.md) 显式选择
线性过滤和 GPU mip 链。该策略属于模型消费与 Render，不改变 ModelResource/Loader 的 CPU 数据契约，
也不表示恢复了源资产的 sampler 设置。
RGB 超范围或非有限、非法结构等错误继续由转换及 ModelResource 构造边界拒绝，不钳制或补默认资产。

Assimp 会压缩未使用的 glTF 顶点，并可能丢弃坏面、三角形余数或错误 UV 流。
导入前检查源 POSITION 数量是否可由 Assimp 表示、层级和会丢失的材质声明；这些修补诊断导致失败，不发布修补后的部分模型。
OBJ 未声明 Kd 时，转换为项目约定的白色；显式 Kd=0.6 保留灰色。
MTL 源检查只识别 Kd 是否声明及不支持的 map_Kd 选项，不另行解析材质数值或实现 MTL 导入器。
选项比较与固定 Assimp 一致，不区分 ASCII 大小写；`-o/-s/-t/-type` 及其大小写变体均拒绝。

## 矩阵、UV 与共享

已有源法线保留方向并归一化，零向量不改为固定方向；非有限分量沿载荷校验及 Loader 的 IO 错误通道拒绝。
缺少法线时，glTF 使用 `aiProcess_GenNormals` 生成逐面平面法线，可能拆分共享顶点、调整索引；
OBJ 使用 `aiProcess_GenSmoothNormals`，最大平滑角固定为 80°，不承诺精确还原所有 OBJ smoothing group。
生成标志按格式二选一，不强制重算已有法线；索引仍为 uint32，拆点不改变面的位置、UV、绕序或材质关联。
退化三角形和未引用顶点的零法线可保留，法线始终处于几何局部空间。

Assimp 矩阵字段 a/b/c/d 表示行，转换时逐元素写入 GLM 列；局部矩阵和源根均不烘焙进顶点。
右手、Y-up 和源数值单位保持不变，不自动居中或归一化。每个 aiMesh 对应一份 MeshData 和一个整网格部分，
节点只保存部分索引；多个源节点引用同一 aiMesh 时继续共享几何。

固定版本先把 glTF 的 V 翻到 Assimp 的左下约定。转换层对 glTF 和 OBJ 输出统一使用 `(u, 1-v)`，
恢复引擎 `(0,0)` 指向图像左上；图片解码、上传和 Shader 不再翻转。投影 Y 适配不属于模型导入。

`incubator_models` 将展示模型和 sidecar 部署到构建资产目录；
本接口只交付 CPU 资产，GPU 依赖与资源由
Runtime 的 [模型准备入口](../../model.md) 处理，实例展开由模型消费层组织。
