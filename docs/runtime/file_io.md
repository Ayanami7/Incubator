# FileIO 模块设计

## 目标
提供平台相关的底层文件 I/O 最小抽象，封装 OS API，为上层资源系统提供可 Mock 的接口。

## 职责边界

| 层次 | 模块 | 职责 |
|------|------|------|
| **Core** | `Path` | 纯字符串路径操作（平台无关，可复用） |
| **Platform** | `FileIO` | OS API 封装 + 最小文件操作抽象（可 Mock） |
| **上层** | `AssetSource` / `ResourceManager` | 虚拟目录映射、排序、类别与资源持有 |

本模块**不会**实现：
- `readAllText` / `readAllBytes`（上层功能，本层只提供基础读写）
- 递归目录索引（本层仅枚举直接子项）
- 路径解析逻辑（由 `core/path` 提供）
- 虚拟文件系统

## 文件结构
```
src/runtime/platform/fileio/
├── fileio.h         # FileIO 主接口 + OpenMode/ShareMode 枚举 + 工厂
├── file_handle.h    # FileHandle 抽象 + SeekOrigin 枚举
└── fileio.cpp       # Windows 平台实现
```

## 枚举定义

### OpenMode 打开模式（位标志组合）

```cpp
enum class OpenMode : uint8_t
{
    Read = 1,
    Write = 2,
    ReadWrite = Read | Write,  // 3
    Create = 4,
    Truncate = 8
};
```

### ShareMode 共享模式

```cpp
enum class ShareMode : uint8_t
{
    Exclusive = 0,
    Read = 1,
    Write = 2,
    ReadWrite = Read | Write  // 3
};
```

### SeekOrigin 定位参考点

```cpp
enum class SeekOrigin : uint8_t
{
    Begin = 0,
    Current = 1,
    End = 2
};
```

### 位运算辅助

```cpp
constexpr OpenMode operator|(OpenMode a, OpenMode b) noexcept;
constexpr OpenMode operator&(OpenMode a, OpenMode b) noexcept;
constexpr bool hasFlag(OpenMode value, OpenMode flag) noexcept;
```

## 接口设计

### FileHandle 文件句柄抽象

```cpp
class FileHandle
{
public:
    virtual ~FileHandle() = default;

    virtual bool isValid() const = 0;                    // 句柄是否有效
    virtual size_t read(std::span<uint8_t> buffer) = 0;  // 读取，返回实际字节数
    virtual size_t write(std::span<const uint8_t> data) = 0;  // 写入，返回实际字节数
    virtual size_t seek(int64_t offset, SeekOrigin origin) = 0;  // 定位，返回新指针位置
    virtual uint64_t tell() const = 0;                   // 当前指针位置
    virtual uint64_t size() const = 0;                   // 文件总字节数
    virtual void close() = 0;                            // 关闭句柄
    virtual void flush() = 0;                            // 刷新缓冲区到物理存储

    // 禁止拷贝，允许移动
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&&) noexcept = default;
    FileHandle& operator=(FileHandle&&) noexcept = default;

protected:
    FileHandle() = default;
};
```

### FileIO 主接口（可 Mock）

```cpp
class FileIO
{
public:
    virtual ~FileIO() = default;

    // 文件操作
    virtual bool exists(const std::filesystem::path& path) = 0;      // 查询文件；其他访问错误抛 IO
    virtual uint64_t getFileSize(const std::filesystem::path& path) = 0;  // 字节数，不存在返回 0
    virtual bool remove(const std::filesystem::path& path) = 0;      // 删除，不存在返回 false

    // 打开文件
    virtual std::unique_ptr<FileHandle> open(
        const std::filesystem::path& path,
        OpenMode mode,
        ShareMode share = ShareMode::Exclusive
    ) = 0;  // 文件不存在返回 nullptr，其他错误抛出 Error::Exception

    // 目录操作
    virtual std::vector<DirectoryEntry> listDirectory(const std::filesystem::path& path) = 0;
    virtual bool directoryExists(const std::filesystem::path& path) = 0;
    virtual bool createDirectory(const std::filesystem::path& path) = 0;  // 不递归创建父目录
    virtual bool removeDirectory(const std::filesystem::path& path) = 0;  // 仅删空目录

protected:
    FileIO() = default;
};

// 工厂：创建平台特定实现
std::unique_ptr<FileIO> createPlatformFileIO();
```

`exists` 在文件存在时返回 true，文件或父路径不存在、目标是目录时返回 false。
Windows 的其他属性查询错误抛 IO，异常包含路径和 Win32 错误码，不把权限等访问失败当作文件缺失。

`DirectoryEntry` 包含 UTF-8 `name`、`directory` 和 `reparsePoint`。`listDirectory` 仅返回直接子项，
不递归、不保证顺序；空目录返回空列表，不存在、不是目录或访问失败抛 `Error::Exception(IO)`。
Windows 实现用目录迭代器枚举，以 `GetFileAttributesW` 取得目录和重解析点标记；不读取文件内容。
虚拟目录的排序及禁止进入目录链接由 AssetSource 处理，FileIO 不承担虚拟路径边界。

## 平台实现（Windows）

**PlatformFileIO / PlatformFileHandle** 在 `fileio.cpp` 中实现，通过工厂函数获取，调用方不直接接触平台类型。

1. **宽字符转换**：路径经 UTF-8 → 宽字符串转换后调用 `CreateFileW` 等 API
2. **API 映射**：`CreateFileW`（打开）、`ReadFile`/`WriteFile`（读写）、`SetFilePointerEx`（seek）、`GetFileSizeEx`（size）、`GetFileAttributesW`（exists）、`CreateDirectoryW`（createDirectory）
3. **错误处理**：`GetLastError()` 转换为 `Error::Exception`；文件不存在等预期场景返回 `false`/`nullptr`
4. **资源管理**：`PlatformFileHandle` 析构时自动 `CloseHandle`

## 使用示例

### 基础文件操作

```cpp
#include "runtime/platform/fileio/fileio.h"

auto fileio = Incubator::FileIO::createPlatformFileIO();

// 检查文件存在
if (fileio->exists("data/config.json"))
{
    uint64_t size = fileio->getFileSize("data/config.json");

    // 打开并读取
    auto handle = fileio->open("data/config.json", OpenMode::Read);
    std::vector<uint8_t> buffer(size);
    handle->read(buffer);
}
```

### 写入文件

```cpp
// 创建/覆盖文件
auto handle = fileio->open("output.log",
                           OpenMode::Create | OpenMode::Write | OpenMode::Truncate);

std::string data = "Hello, World!";
handle->write(std::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(data.data()), data.size()));
handle->close();
```

### 使用 Mock 进行测试

```cpp
// 测试中的 Mock 实现
class MockFileIO : public FileIO
{
    // 可以注入假文件数据，记录调用等
};

void testResourceLoader()
{
    auto mockFileIO = std::make_unique<MockFileIO>();
    // ... 配置 mock
    // ResourceLoader loader(std::move(mockFileIO));
    // ... 测试逻辑
}
```

## 上层实现示例（不在本模块）

```cpp
// 上层功能：readAllBytes
std::vector<uint8_t> readAllBytes(FileIO& fileio, std::string_view path)
{
    uint64_t size = fileio.getFileSize(path);
    auto handle = fileio.open(path, OpenMode::Read);
    std::vector<uint8_t> buffer(size);
    handle->read(buffer);
    return buffer;
}
```

## 测试要点
- FileIO 接口的 Mock 能力
- 文件打开/读写的正确性
- 错误路径的异常处理
- Windows 句柄泄漏检查
- UTF-8 路径支持
- 文件共享模式的行为
