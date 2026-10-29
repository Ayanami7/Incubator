# Resource 资源系统

## 目标

为引擎提供**全局稳定资源标识**、**资源注册/去重**与**同步加载**能力。子系统沿
`ResourceId（身份）→ ResourceRegistry（登记）→ ResourceManager（门面）→ Loader（桥）→ ResourceObject（实例）`
分层，覆盖从"资源身份"到"可用的运行时对象"的完整链路。

## 分层图

```
ResourceId ──► ResourceRegistry ──► ResourceManager ──► Loader ──► ResourceObject
  身份             登记                 门面               桥              实例
 稳定ID       去重/碰撞/计数/挂载      Loader路由+加载      磁盘→对象         运行时载荷
```

- **身份**：`(类型, 规范化虚拟路径)` 哈希，跨会话稳定
- **登记**：全局户口本，去重、哈希碰撞检测、引用计数、运行时对象唯一挂载点
- **门面**：Loader 路由、同步加载、生命周期（acquire/release）与异常边界
- **桥**：把磁盘数据解析并构建为 ResourceObject，按 ResourceType 路由
- **实例**：薄基类承载 id/type/state，具体载荷由派生类持有

## 组件索引

[DemoApp 示例](../../examples/demo.md) 沿用这些全局组件，独占 Loader 路由及资产源绑定，并持有 AssetSource/FileIO 至关闭；
CPU 资产 release 与路由销毁分别管理，运行期间可再次显式准备模型，不将 Manager 改为私有实例。
[Scene Runtime 示例](../../examples/demo.md) 在打开场景或创建实体时准备所需模型，随后通过会话缓存复用结果。

| 组件 | 文档 | 状态 |
|---|---|---|
| ResourceType 封闭枚举 | [resource_type.md](./resource_type.md) | ✅ 已实现 |
| ResourceId + 虚拟路径规范化 | [resource_id.md](./resource_id.md) | ✅ 已实现 |
| ResourceRegistry + Entry（X3） | [resource_registry.md](./resource_registry.md) | ✅ 已实现 |
| AssetInfo 描述壳 | [asset_info.md](./asset_info.md) | ✅ 已实现 |
| ResourceObject 薄基类 + ResourceState | [resource_object.md](./resource_object.md) | ✅ 已实现 |
| ResourceManager 门面 | [resource_manager.md](./resource_manager.md) | ✅ 已实现 |
| LoaderBase + 虚拟路径解析约定 | [loader/loader.md](./loader/loader.md) | ✅ 已实现 |
| TextLoader 首个实现 | [loader/text_loader.md](./loader/text_loader.md) | ✅ 已实现 |
| ShaderResource + ShaderLoader（SPIR-V CPU 载荷） | [loader/shader_loader.md](./loader/shader_loader.md) | ✅ 已实现 |
| TextureResource + TextureLoader（PNG / RGBA8 CPU 载荷） | [loader/texture_loader.md](./loader/texture_loader.md) | ✅ 已实现 |
| MeshData + ModelResource（CPU 几何、材质与节点组合） | [model_resource.md](./model_resource.md) | ✅ 已实现 |
| ModelLoader（Assimp 静态 glTF / OBJ 导入） | [loader/model_loader.md](./loader/model_loader.md) | ✅ CPU 导入已实现；GPU 消费见 [Model](../model.md) |

## 文件布局（镜像 src）

```
src/runtime/resource/
├── resource_type.h
├── resource_id.h/.cpp
├── resource_state.h
├── asset_info.h
├── resource_object.h/.cpp
├── resource_registry.h/.cpp
├── resource_manager.h/.cpp
├── mesh_data.h
└── loader/
    ├── loader_base.h
    ├── text_loader.h/.cpp
    ├── text_resource.h/.cpp
    ├── spirv_reader.h/.cpp
    ├── shader_loader.h/.cpp
    ├── shader_resource.h/.cpp
    ├── texture_loader.h/.cpp
    ├── texture_resource.h/.cpp
    ├── model_resource.h/.cpp
    ├── model_loader.h/.cpp
    └── detail/                    # 私有 Assimp IO 与源输入检查
```

ShaderLoader 复用共享的 `Loader::readSpirv` 二进制入口，不引入 Vulkan 依赖。ResourceManager 管理 ShaderResource 的 CPU 载荷；Renderer 在 `prepareSceneResources` 中复制字载荷，用于首次创建及交换链兼容 Pipeline 重建。ResourceManager 释放 CPU 对象不触发 Vulkan 资源销毁。

TextureLoader 经 FileIO 完整读取并在内存解码 PNG，TextureResource 拥有固定 sRGB RGBA8 像素。
应用将其借给 `Renderer::createTexture` 同步上传，调用成功后可 release 到零；GPU Texture 与在途资源由 Render
独立持有。资产源构造时捕获绝对根，默认 cwd/assets，读取本身不依赖后续 cwd。

ModelResource 使用 MODEL 类型，拥有独立的 MeshData（含局部法线）、线性基色/外部纹理路径及源 Unlit 描述、几何与材质关联，
以及显式源根和节点局部矩阵。构造时校验几何和单根树结构，之后只读访问；多个节点可复用同一几何。
ModelLoader 通过现有 FileIO 和固定 Assimp 导入外部 bin/PNG 的静态 glTF 2.0、外部 mtl/PNG 的 OBJ，
返回自有 CPU 资产，图片仅保留路径。GPU 资源准备和实例绘制由 [Model 消费层](../model.md)
基于 Resource 与 Render 完成，Resource 本身仍负责 CPU 资源。数据及矩阵约定见
[ModelResource](./model_resource.md)，格式与材质支持范围见 [ModelLoader](./loader/model_loader.md)。

## 单目录资产源

`AssetSource(FileIO&, root = "assets")` 是具体目录源，借用文件服务并在构造时捕获绝对根。
resolve/open/exists 接收相对 UTF-8 文件路径；真实物理根不进入 ResourceId、ModelReference 或场景保存。
Text/Shader/Texture/Model Loader 都借用同一源，Assimp 主文件及 bin/mtl 与后续 PNG 准备使用相同根。
源保证非空相对文件路径的词法边界；根视为本地受信任目录，不承诺链接解析后的文件访问隔离。
`ResourceManager::listDirectory()` 列出根目录，传相对虚拟路径列出子目录；每次重新枚举直接子项并按规范化路径排序。
目录链接和目录重解析点从结果中排除，直接请求其后代路径也被拒绝。空目录返回空列表，枚举失败抛 IO。
OBJ/glTF、PNG、bin/mtl 分别标为模型、纹理和依赖候选，其他项为普通文件；分类不代表内容已验证。
列表和 `query(type, path)` 不登记、不加载、不改变引用；CPU 已加载状态与文件发现、GPU 准备相互独立。
普通消费者通过 `hold<T>(type, path)` 取得只可移动的 `CpuResource`，使用期间借用 `get<T>()`，析构或 reset 释放本次持有。
失败值的 `failure()` 保存本次调用的虚拟路径及具体原因；后续调用不会覆盖它。详见 [ResourceManager](./resource_manager.md)。

会话先构造 FileIO 和 AssetSource，再 bindAssetSource(&source) 并注册 Loader。
结束时释放全部 CPU 持有和载荷，reset 清除路由，再 bindAssetSource(nullptr)，最后销毁源及 FileIO。
换绑或解绑时仍有路由、CPU 持有或挂载对象会抛 CONFIG，原状态保持；零持有、无载荷的身份登记可以保留。
同一进程后续绑定其他根时，旧登记仍使用相同 ID，但载荷必须重新从新根读取。
Registry::hasLiveResources 检查持有或挂载状态，不强制销毁外部对象；reset 仍只清路由。
场景文件、启动配置、布局和字体的现有 FileIO/cwd 规则保持不变。

## 演进路线（未实现项触发条件）

| 项目 | 触发条件 |
|---|---|
| 异步加载（启用 LOADING/ERROR 态） | 出现跨帧加载或后台流式加载需求 |
| 热重载（contentHash 变更检测） | 需要运行时资源迭代的工作流 |
| VFS（虚拟路径 → 真实文件解析 / 打包） | .pak 打包、多资源根需求 |
| ResourceHandle / LOD / 流式加载 | 一个 Asset 多实例需求时引入 |
| 异步加载、热重载、Cook、资产清单 | 按具体资源管线需求另行规划 |

## 设计原则

1. **身份稳定**：ID 由 (类型, 规范化虚拟路径) 哈希，不依赖运行时顺序，跨会话可复现
2. **单一真源**：虚拟路径唯一真源在 AssetInfo；refCount 与对象挂载在 Registry::Entry
3. **封闭类型**：ResourceType 为封闭枚举，同时充当类型 tag 与 Loader 路由键
4. **失败走返回值**：预期失败（未注册/无 Loader/文件缺失）返回 `nullptr`；Loader 抛异常由 Manager 门面统一捕获，`load()` 保证不向外抛异常
5. **同步原语**：当前即用即得；LOADING/ERROR 状态为异步预留
