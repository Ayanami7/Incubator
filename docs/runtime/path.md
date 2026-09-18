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
    bool hasRootName(std::string_view path);
    bool hasExtension(std::string_view path, std::string_view ext);

    // 字符串工具
    std::string toLowerASCII(std::string_view s);
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

### hasRootName
判断是否包含**名称根**（root-name），比 isAbsolute 更严格：只要存在盘符或 UNC 前缀即返回 true，**包含仅含盘符根名、根目录为空的 drive-relative 路径**（如 `C:foo`，此时 `is_absolute()` 为 false）。
```cpp
Path::hasRootName("folder/file.txt")         // false
Path::hasRootName("C:foo")                   // true (drive-relative，无根目录)
Path::hasRootName("C:/folder/file.txt")      // true (盘符前缀)
Path::hasRootName("//server/share/file.txt") // true (UNC 路径，MSVC generic 格式需手动检测)
```

用途：判定路径是否携带"宿主文件系统"信息，用于虚拟路径 / 资源键等必须相对的场景。

### hasExtension
```cpp
Path::hasExtension("file.txt", "txt")      // true
Path::hasExtension("file.txt", ".txt")     // true
Path::hasExtension("file.TXT", "txt")      // true (大小写不敏感)
```

### toLowerASCII
**仅折叠 ASCII 大写字母 `A-Z` 为小写，非 ASCII 字符（如中文）与数字/符号保持不变**：
```cpp
Path::toLowerASCII("Textures/Player.PNG")  // "textures/player.png"
Path::toLowerASCII("中文/纹理")             // "中文/纹理"（非 ASCII 不变）
Path::toLowerASCII("Tex123!_@#")           // "tex123!_@#"（数字/符号不变）
```

用途：生成大小写无关的资源键（配合资源 ID 使用），在 Windows/macOS 等大小写不敏感的文件系统上保证同一资产的不同大小写拼写映射到同一 ID。

## 平台处理

### Windows
- 绝对路径检测：`C:`, `D:` 等盘符开头，或 `//` UNC 路径
- 名称根检测（`hasRootName`）：盘符前缀（含 drive-relative 如 `C:foo`）与 UNC 前缀均命中
- 分隔符：内部统一使用 `/`，但可解析 `\` 分隔符

## 实现要点

1. **分隔符统一使用 `/`** - normalize 会将 `\` 替换为 `/`
2. **大小写不敏感** - 扩展名比较时忽略大小写；toLowerASCII 仅折叠 ASCII（避免 locale 相关的不确定性）
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
- toLowerASCII 的 ASCII 折叠与非 ASCII 不变
- 无系统调用
