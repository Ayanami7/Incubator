# FileIO 模块设计

## 目标
提供平台相关的底层文件 I/O 最小抽象，封装 OS API，为上层资源系统提供可 Mock 的接口。

## 职责边界

| 层次 | 模块 | 职责 |
|------|------|------|
| **Core** | `Path` | 纯字符串路径操作（平台无关，可复用） |
| **Platform** | `FileIO` | OS API 封装 + 最小文件操作抽象（可 Mock） |
| **上层** | （待设计） | 高级功能：readAllBytes、目录遍历、VFS 等 |

本模块**不会**实现：
- `readAllText` / `readAllBytes`（上层功能，本层只提供基础读写）
- 目录遍历 API（可用本层的基础接口实现）
- 路径解析逻辑（由 `core/path` 提供）
- 虚拟文件系统

## 文件结构
```
src/runtime/
├── core/path/
│   └── path.h           # 纯路径操作（平台无关）
└── platform/fileio/
    ├── fileio.h         # 主接口 + 抽象基类
    ├── file_handle.h    # 文件句柄抽象
    └── fileio.cpp       # Windows 平台实现
```

## Core/Path 设计

**纯静态工具类，平台无关的字符串操作：**

```cpp
namespace Path {
    std::string join(std::string_view a, std::string_view b);
    std::string normalize(std::string_view path);
    std::string getExtension(std::string_view path);
    std::string getStem(std::string_view path);
    std::string getFileName(std::string_view path);
    std::string getParentPath(std::string_view path);
    bool isAbsolute(std::string_view path);
}
```

## Platform/FileIO 设计

### 抽象接口（可 Mock）

**FileHandle - 文件句柄抽象：**
```cpp
class FileHandle {
public:
    virtual ~FileHandle() = default;
    virtual bool isValid() const = 0;
    virtual size_t read(std::span<uint8_t> buffer) = 0;
    virtual size_t write(std::span<const uint8_t> data) = 0;
    virtual size_t seek(int64_t offset, SeekOrigin origin) = 0;
    virtual uint64_t tell() const = 0;
    virtual uint64_t size() const = 0;
    virtual void close() = 0;
};
```

**FileIO - 主接口（可 Mock）：**
```cpp
class FileIO {
public:
    virtual ~FileIO() = default;

    // 文件操作
    virtual bool exists(std::string_view path) = 0;
    virtual uint64_t getFileSize(std::string_view path) = 0;
    virtual bool remove(std::string_view path) = 0;

    // 文件打开
    virtual std::unique_ptr<FileHandle> open(
        std::string_view path,
        OpenMode mode,
        ShareMode share = ShareMode::Exclusive
    ) = 0;

    // 目录操作
    virtual bool directoryExists(std::string_view path) = 0;
    virtual bool createDirectory(std::string_view path) = 0;
    virtual bool removeDirectory(std::string_view path) = 0;
};
```

**枚举定义：**
```cpp
enum class OpenMode : uint8_t {
    Read = 1,
    Write = 2,
    ReadWrite = 3,
    Create = 4,
    Truncate = 8
};

enum class ShareMode : uint8_t {
    Exclusive = 0,
    Read = 1,
    Write = 2,
    ReadWrite = 3
};

enum class SeekOrigin : uint8_t {
    Begin = 0,
    Current = 1,
    End = 2
};
```

### 平台实现

**PlatformFileIO - Windows 实现：**
```cpp
class PlatformFileIO : public FileIO {
    // 实现 FileIO 所有接口
};
```

**PlatformFileHandle - Windows 文件句柄：**
```cpp
class PlatformFileHandle : public FileHandle {
public:
    explicit PlatformFileHandle(HANDLE handle);
    ~PlatformFileHandle() override;

    // 实现 FileHandle 所有接口
private:
    HANDLE m_handle;
};
```

## 使用示例

### 基础文件操作
```cpp
#include "platform/fileio/fileio.h"
#include "core/path/path.h"

// 使用默认平台实现
auto fileio = std::make_unique<PlatformFileIO>();

// 检查文件存在
if (fileio->exists("data/config.json")) {
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
auto handle = fileio->open("output.log", OpenMode::Create | OpenMode::Write | OpenMode::Truncate);

// 写入数据
std::string data = "Hello, World!";
handle->write(std::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(data.data()),
    data.size()
));

handle->close();
```

### 使用 Mock 进行测试
```cpp
// 测试中的 Mock 实现
class MockFileIO : public FileIO {
    // 可以注入假文件数据，记录调用等
};

void testResourceLoader() {
    auto mockFileIO = std::make_unique<MockFileIO>();
    // ... 配置 mock
    ResourceLoader loader(std::move(mockFileIO));
    // ... 测试逻辑
}
```

## 平台实现要点（Windows）

1. **宽字符转换**:
   ```cpp
   std::wstring toWideString(std::string_view utf8);
   std::string fromWideString(std::wstring_view wide);
   ```

2. **API 映射**:
   - `CreateFileW` → 打开文件
   - `ReadFile` / `WriteFile` → 读写
   - `SetFilePointerEx` → seek
   - `GetFileSizeEx` → size
   - `GetFileAttributesW` → exists
   - `CreateDirectoryW` → createDirectory

3. **错误处理**:
   - 使用 `GetLastError()` 转换为 `Exception`
   - 返回 `false` 表示操作失败（调用者可继续）

4. **资源管理**:
   - `PlatformFileHandle` 析构时自动调用 `CloseHandle`

## 上层实现示例（不在本模块）

使用 FileIO 实现高级功能：

```cpp
// 上层功能：readAllBytes
std::vector<uint8_t> readAllBytes(FileIO& fileio, std::string_view path) {
    uint64_t size = fileio.getFileSize(path);
    auto handle = fileio->open(path, OpenMode::Read);

    std::vector<uint8_t> buffer(size);
    handle->read(buffer);
    return buffer;
}

// 上层功能：目录遍历
std::vector<std::string> listFiles(FileIO& fileio, std::string_view path) {
    // 使用 FileIO + Path 组合实现
    // Windows 下可用 FindFirstFileW / FindNextFileW
}
```

## 测试要点
- FileIO 接口的 Mock 能力
- 文件打开/读写的正确性
- 错误路径的异常处理
- Windows 句柄泄漏检查
- UTF-8 路径支持
- 文件共享模式的行为
