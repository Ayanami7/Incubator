# Path 模块设计

## 目标
提供纯字符串的路径操作逻辑，可复用于任何需要路径处理的场景。

## 设计原则

1. **纯逻辑，无系统调用** - 仅操作字符串，不访问文件系统
2. **无状态** - 所有方法为静态函数
3. **最小依赖** - 仅依赖标准库

## 文件结构
```
src/runtime/core/path/
└── path.h
```

## 接口设计

```cpp
namespace Path {
    // 路径连接
    std::string join(std::string_view a, std::string_view b);
    std::string join(std::string_view a, std::string_view b, std::string_view c);

    // 路径规范化：处理 . 和 ..，统一分隔符
    std::string normalize(std::string_view path);

    // 组件提取
    std::string getFileName(std::string_view path);
    std::string getStem(std::string_view path);
    std::string getExtension(std::string_view path);
    std::string getParentPath(std::string_view path);

    // 路径判断
    bool isAbsolute(std::string_view path);
    bool hasExtension(std::string_view path, std::string_view ext);
}
```

## 接口说明

### join
**强制使用 `/` 作为分隔符**：
```cpp
Path::join("folder", "file.txt")           // "folder/file.txt"
Path::join("folder/", "file.txt")          // "folder/file.txt"（自动去重）
Path::join("C:/dir", "sub/file.txt")      // "C:/dir/sub/file.txt"
```

### normalize
**强制统一分隔符为 `/`**：
```cpp
Path::normalize("a/../b")                  // "b"
Path::normalize("a/./b")                   // "a/b"
Path::normalize("a//b")                    // "a/b"（去除多余分隔符）
```

### getFileName / getStem / getExtension
```cpp
Path::getFileName("folder/file.txt")       // "file.txt"
Path::getStem("folder/file.txt")           // "file"
Path::getExtension("folder/file.txt")      // "txt"
Path::getExtension("folder/file")          // ""
Path::getFileName("folder/")               // ""
```

### getParentPath
```cpp
Path::getParentPath("folder/file.txt")     // "folder"
Path::getParentPath("folder/")             // ""
Path::getParentPath("folder")              // ""
```

### isAbsolute
```cpp
Path::isAbsolute("folder/file.txt")        // false
Path::isAbsolute("C:/folder/file.txt")     // true (Windows 盘符)
Path::isAbsolute("//server/share/file.txt") // true (UNC 路径)
```

### hasExtension
```cpp
Path::hasExtension("file.txt", "txt")      // true
Path::hasExtension("file.txt", ".txt")     // true
Path::hasExtension("file.TXT", "txt")      // true (大小写不敏感)
```

## 平台处理

### Windows
- 绝对路径检测：`C:`, `D:` 等盘符开头，或 `//` UNC 路径
- 分隔符：内部统一使用 `/`，但可解析 `\` 分隔符

## 实现要点

1. **分隔符统一使用 `/`** - normalize 会将 `\` 替换为 `/`
2. **大小写不敏感** - 扩展名比较时忽略大小写
3. **空路径处理** - 所有函数对空字符串有明确定义
4. **无系统调用** - 所有操作均为纯字符串处理

## 使用示例

```cpp
#include "core/path/path.h"

// 资源路径构建
std::string assetPath = Path::join("assets", "textures", "player.png");

// 路径规范化
std::string cleanPath = Path::normalize("assets/../textures/./player.png");

// 扩展名判断
if (Path::hasExtension(file, "png") || Path::hasExtension(file, "jpg")) {
    // 处理图片
}
```

## 测试要点
- join 的边界情况（空路径、已存在分隔符）
- normalize 的正确性（. 和 .. 处理）
- Windows 盘符和 UNC 路径处理
- 大小写不敏感的扩展名匹配
- 无系统调用
