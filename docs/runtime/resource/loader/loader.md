# Loader 桥模块设计

## 目标

把磁盘数据**解析并构建为 ResourceObject**。Loader 是资源层的"桥"：
`AssetInfo → Loader::load → ResourceObject`，按 `ResourceType` 路由（封闭枚举的用处）。
Loader 直接站在已有的 `FileIO`（OS 抽象）+ `Path`（字符串操作）之上组合，**不新建文件系统层**。

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

- Loader 内 `Path::join("assets", virtualPath)` 得到真实相对路径，**不建 VFS**；相对进程工作目录解析
- 引擎路径字符串按 UTF-8 处理。把 `Path::join` 的结果交给 FileIO 前，必须通过 `Utils::Utf8::toFsPath` 构造 `std::filesystem::path`；诊断输出物理路径时使用 `Utils::Utf8::toUtf8String`。不得依赖 `std::string` 到 `filesystem::path` 的隐式转换或 `generic_string()`。
- 虚拟路径已在登记边界规范化（拒绝盘符/UNC/越界 `..`），Loader 端无需重复处理文本逃逸
- 首版资源根固定为进程启动工作目录下的 `assets/`。Demo 从构建目录启动，故读取 `<build>/assets/`；工作目录在资源被持有或缓存对象存在期间保持稳定。开发调试和终端启动命令见 [Task 02](../../../../.agents/specs/tasks/2026-10-13-texture-loading/task-02-asset-path.md)。
- **VFS 触发条件**：出现多资源根、`.pak` 打包、归档需求时，在 Loader 与 FileIO 之间插入 VFS 层（增量扩展，当前 YAGNI）

## FileIO 注入

Loader 通过构造函数注入 `Incubator::FileIO::FileIO&`（如 `TextLoader(FileIO&)`），而非内部自建实例：

- 生产与测试共用同一套真实 FileIO（`createPlatformFileIO()`）
- 测试用临时目录替换工作目录即可（虚拟路径 join 后落在临时目录），无需 Mock 文件系统

## 新增一个 Loader 的模板

1. 新建 `<name>_loader.h/.cpp`（继承 `LoaderBase`，`final`）
2. 若引入新载荷类型，另建 `<name>_resource.h/.cpp`（继承 `ResourceObject`，`final`）；载荷可复用则跳过
3. 若为新资源种，在 `ResourceType` 增量添加显式 `constHash` 枚举值（需重编译）
4. `CMakeLists.txt` 登记头文件/源文件
5. 编写测试：真实 FileIO + 临时目录模式（参照 `text_loader_test.cpp`）

## 文档索引

- [text_loader.md](./text_loader.md) - 首个实现（文本资源全链路）
- [shader_loader.md](./shader_loader.md) - SPIR-V CPU 载荷与异常门面接通
