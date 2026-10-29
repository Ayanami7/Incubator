# Loader 桥模块设计

## 目标

把磁盘数据**解析并构建为 ResourceObject**。Loader 是资源层的"桥"：
`AssetInfo → Loader::load → ResourceObject`，按 `ResourceType` 路由（封闭枚举的用处）。
Loader 借用同一具体目录 `AssetSource`，由它将虚拟文件路径映射到绝对物理根，再通过已有 FileIO 读取；不建立挂载后端或第二套文件系统。

## LoaderBase 接口

```cpp
class LoaderBase
{
public:
    virtual ~LoaderBase() = default;

    // 加载并构建 ResourceObject；失败可返回 nullptr 或透传加载异常
    virtual std::unique_ptr<ResourceObject> load(const ResourceId& id,
                                                 std::string_view virtualPath) = 0;
};
```

- 按 `ResourceType` 注册到 `ResourceManager` 的路由表（`registerLoader`，同类型覆盖）
- 参数：`id`（Registry 已登记）、`virtualPath`（AssetInfo 持有的规范化虚拟路径）
- 成功返回的对象 state 由 `ResourceManager` 置为 READY（Loader 不负责状态）

## 失败约定

- **返回 nullptr 表示预期失败**（文件缺失等），Loader 内部应记录 error 日志
- **异常失败**：读取/解析异常可透传。经 ResourceManager 加载时，Manager 记录一次错误并转换为 nullptr，Loader 无需重复捕获或记录同一异常。
- **直接调用**：调用方遵循 Loader 的可抛异常契约并处理失败。Manager 的不抛异常门面语义不改变 Loader 本身的契约。

## 虚拟路径解析最小约定

- Loader 通过 AssetSource::resolve/open 访问根内文件，不再自行拼接 assets 前缀。
- 引擎路径字符串按 UTF-8 处理；AssetSource 校验非空相对文件路径、拒绝绝对/网络/词法越根路径，物理路径经 UTF-8 工具转换。不承诺文件链接解析后的物理隔离。
- 虚拟路径已在登记边界规范化（拒绝盘符/UNC/越界 `..`），Loader 端无需重复处理文本逃逸
- 模型内新声明的 sidecar/纹理路径属于新的输入边界，由 ModelLoader 按声明文件目录解析并检查根内范围；不继承模型登记路径的合法性保证。
- AssetSource 构造时捕获绝对根，默认由当时 cwd/assets 解析，也可显式指定目录；后续文件访问不依赖 cwd。Demo 的场景、配置、布局和字体仍按原 cwd 约定处理。
- **VFS 触发条件**：出现多资源根、`.pak` 打包、归档需求时，在 Loader 与 FileIO 之间插入 VFS 层（增量扩展，当前 YAGNI）

## FileIO 注入

Loader 构造函数注入 `AssetSource&`（例如 `TextLoader(source)`）；源借用消费者的 FileIO：

- 生产与测试共用同一套真实 FileIO（`createPlatformFileIO()`）
- 顺序为释放全部 CPU 持有及载荷 → reset 清除 Loader → bindAssetSource(nullptr) 解除绑定 → 销毁源 → 销毁 FileIO。reset 不替代 release；未清理完毕时更换或解除绑定抛 CONFIG，并保留原绑定及对象。
- Demo 模型模式在此作用域内完成 ModelLoader 导入及 Model::prepareModel 的 PNG 依赖加载，返回的 PreparedModel 不借用 Loader 或 FileIO；最终用法见 [Demo](../../../examples/demo.md#启动时选择模型)。
- 文件定位测试用临时目录替换工作目录（虚拟路径 join 后落在临时目录）；短读、提前 EOF、读取异常与输入长度上限测试通过受控 FileIO/FileHandle 注入，不实际分配巨量文件。

## 新增一个 Loader 的模板

1. 新建 `<name>_loader.h/.cpp`（继承 `LoaderBase`，`final`）
2. 若引入新载荷类型，另建 `<name>_resource.h/.cpp`（继承 `ResourceObject`，`final`）；载荷可复用则跳过
3. 若为新资源种，在 `ResourceType` 增量添加显式 `constHash` 枚举值（需重编译）
4. `CMakeLists.txt` 登记头文件/源文件
5. 编写测试：真实 FileIO + 临时目录模式（参照 `text_loader_test.cpp`）

## 文档索引

- [text_loader.md](./text_loader.md) - 首个实现（文本资源全链路）
- [shader_loader.md](./shader_loader.md) - SPIR-V CPU 载荷与异常门面接通
- [texture_loader.md](./texture_loader.md) - PNG 完整读取、RGBA8 CPU 载荷与失败边界
- [model_loader.md](./model_loader.md) - Assimp 经 FileIO 导入静态 glTF/OBJ，输出模型自有 CPU 数据
