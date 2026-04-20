# Error 模块设计

## 目标
提供统一的错误处理机制，基于 `std::source_location` 自动捕获调用上下文。

## 文件结构
```
src/runtime/core/error/
├── error.h
└── error.cpp
```

## 核心组件

### ErrorCode 枚举
定义标准的错误代码分类：
- `UNKNOWN`: 未知错误
- `IO`: 文件/输入输出错误
- `CONFIG`: 配置错误
- `NETWORK`: 网络错误

### Exception 类
继承自 `std::exception`，提供以下接口：

**构造函数：**
```cpp
Exception(ErrorCode code, std::string message, std::source_location loc = std::source_location::current())
```

**访问方法：**
- `const char* what() const noexcept`: 返回完整的错误信息（包含位置）
- `ErrorCode code() const noexcept`: 获取错误代码
- `const std::string& message() const noexcept`: 获取用户定义的消息
- `const std::source_location& location() const noexcept`: 获取调用位置

**辅助工具：**
- `toString(ErrorCode code)`: 错误代码转字符串
- `getFileName(std::string_view path)`: 从完整路径提取文件名

## 使用示例

```cpp
// 抛出异常（自动捕获调用位置）
throw Exception(ErrorCode::IO, "Failed to open file");

// 捕获并处理异常
try {
    // ... 代码
} catch (const Exception& e) {
    auto& loc = e.location();
    logError(logger, e, LogLevel::ERROR);
}
```

## 设计要点
1. **自动位置捕获**: 使用 `source_location::current()` 在构造时记录调用位置
2. **延迟构建**: `what()` 方法使用缓存机制，避免重复构建字符串
3. **线程安全**: 异常对象在异常传播期间是线程隔离的

## 测试要点
- 异常构造时的位置信息准确性
- `what()` 返回的字符串格式正确性
- 错误代码与消息的正确关联
