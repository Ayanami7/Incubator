# String Utilities 模块设计

## 目标
提供字符串处理工具，用于路径操作、编码转换等。

## 文件结构
```
src/runtime/core/utils/
├── string.h
└── string.cpp
```

## 接口设计

```cpp
namespace Incubator::Utils
{
    // 路径处理
    std::string getParentPath(std::string_view path);
    std::string getFileName(std::string_view path);
    std::string getExtension(std::string_view path);
    std::string joinPath(std::string_view base, std::string_view relative);
    bool isAbsolutePath(std::string_view path);

    // 字符串工具
    bool startsWith(std::string_view str, std::string_view prefix);
    bool endsWith(std::string_view str, std::string_view suffix);
    std::vector<std::string> split(std::string_view str, char delimiter);
    std::string trim(std::string_view str);
    std::string toLower(std::string_view str);
    std::string toUpper(std::string_view str);
    bool equalsIgnoreCase(std::string_view a, std::string_view b);

    // 格式化（已有 fmt，可提供便捷封装）
    template <typename... Args>
    std::string format(fmt::format_string<Args...> fmt, Args&&... args);
}
```

## 测试要点
- 路径处理的跨平台兼容性（Windows/Linux 路径分隔符）
- 空字符串和边界情况
