# Error 模块设计

## 目标
提供统一的错误处理机制，基于 `std::source_location` 自动捕获调用上下文。

## 文件结构
```
src/runtime/core/error/
├── error.h
└── error.cpp
```

## 命名空间

```cpp
namespace Incubator::Error
```

## 核心组件

### Code 枚举

```cpp
enum class Code
{
    UNKNOWN,  ///< 未知/未分类错误
    IO,       ///< 文件/输入输出错误
    CONFIG,   ///< 配置解析错误
    NETWORK   ///< 网络通信错误
};
```

定义引擎中可能出现的错误分类，用于 `Exception` 构造和上层错误处理。

### Exception 类

继承自 `std::exception`，构造时自动捕获调用位置（文件名、行号、函数名）。

```cpp
class Exception : public std::exception
{
public:
    // 构造异常（loc 默认自动捕获当前调用点）
    Exception(Code code, std::string message,
              std::source_location loc = std::source_location::current());

    // 获取格式化错误信息：包含错误代码、消息与调用位置的完整字符串
    const char* what() const noexcept override;

    Code code() const noexcept;                      // 错误代码
    const std::string& message() const noexcept;     // 用户定义的消息
    const std::source_location& location() const noexcept;  // 调用位置
};
```

### 辅助函数

```cpp
std::string toString(Code code);   // Code → 字符串（如 Code::IO → "IO"）
```

## 使用示例

```cpp
// 抛出异常（自动捕获调用位置）
throw Error::Exception(Error::Code::IO, "Failed to open file");

// 捕获并处理异常
try
{
    // ... 代码
}
catch (const Error::Exception& e)
{
    Log::logError(logger, e, Log::Level::ERROR);  // 将异常输出到日志
}
```

## 实现要点

1. **自动位置捕获**：构造函数默认参数 `std::source_location::current()` 在调用点记录位置
2. **一次性构建**：`what()` 的完整字符串在构造时构建一次（`what_cache_`），后续调用零重复开销
3. **文件名精简**：`what()` 中只保留路径的最后一段文件名，避免输出完整绝对路径

### what() 输出格式

```
[CODE]  message  @file(line:function_name)
```

示例：
```
[IO]  Failed to open file  @fileio.cpp(45:openFile)
```

## 设计定位

- 异常用于**致命/不可恢复**信号（文件打开失败、配置错误等），调用方捕获统一处理
- **预期失败的场景不使用异常**（如"查找不存在"），此类接口返回 `bool` / 无效值（见 ResourceRegistry）

## 测试要点
- 异常构造时的位置信息准确性
- `what()` 返回的字符串格式正确性
- 错误代码与消息的正确关联