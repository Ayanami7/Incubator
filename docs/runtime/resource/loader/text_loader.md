# TextLoader 首个实现

## 目标

落地第一个真实 Loader，验证 `AssetInfo → ResourceManager::load → TextLoader（FileIO 读盘）→ TextResource → Entry 挂载` 全链路。

## 载荷：TextResource

```cpp
class TextResource final : public ResourceObject
{
public:
    TextResource(ResourceId id, std::string text);
    const std::string& text() const noexcept;   // 文本内容
};
```

文本资源是配置/JSON/着色器源码的通用载荷。声明 `final`：引擎对具体载荷不做进一步分发。

## Loader 实现

```cpp
class TextLoader final : public LoaderBase
{
public:
    explicit TextLoader(Incubator::FileIO::FileIO& fileio);
    std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view virtualPath) override;
};
```

`text_loader.cpp` 流程：

1. `Path::join("assets", virtualPath)` → 真实相对路径（相对进程工作目录）
2. `fileio.getFileSize(realPath)` 取大小（文件不存在 → 0）
3. `fileio.open(realPath, OpenMode::Read)` → 文件不存在返回 `nullptr`（预期失败路径）；权限等异常为 `Error::Exception`
4. `std::string text(size, '\0')` + `handle->read(std::span<uint8_t>(...))` 读入
5. `text.resize(read)` 遵循实际读取字节数（支持部分读）
6. `std::make_unique<TextResource>(id, std::move(text))` 返回

## 失败语义

- **文件缺失**：`open` 返回 nullptr → `error` 日志 + 返回 `nullptr`（不抛异常）
- **空文件**：size 0 → 空字符串 `TextResource`（合法结果）
- **权限/设备等可恢复错误**：`Error::Exception` 透传，由 Manager 门面统一捕获转 `nullptr`（本 Loader 不包 try/catch）
- 返回 `nullptr` 时不会挂载任何对象，条目保持未加载态，可再次尝试

## 使用样例

```cpp
auto fileio = Incubator::FileIO::createPlatformFileIO();
auto& manager = ResourceManager::instance();
manager.registerLoader(ResourceType::TEXT, std::make_unique<TextLoader>(*fileio));

ResourceId id = manager.registerResource(ResourceType::TEXT, "settings/config.txt");
TextResource* res = manager.get<TextResource>(id);   // load + dynamic_cast
std::string_view content = res->text();

manager.release(id);                                  // 归零即卸载，登记保留
```

## 测试要点

- 真实 FileIO + **临时目录**模式：`SetUp` 中 `fs::current_path(testDir_)` 切换工作目录（join 出的 `assets/...` 落到临时目录），`TearDown` 恢复删除
- 覆盖：全链路加载、虚拟路径解析、二次加载复用（组合式 `CountingLoader` 计数）、文件缺失、无 Loader、
  错误类型返回 null、释放归零卸载但登记保留、释放后重建（`ReloadAfterUnloadRebuildsObject`）
- 组合式计数而非子类化 `TextLoader`（`final` 类不可派生）