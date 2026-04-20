# Logger 模块设计

## 目标
基于 spdlog 提供高性能、模块化的日志系统，支持多 sink 和异步日志。

## 文件结构
```
src/runtime/core/log/
├── logger.h
└── logger.cpp
```

## 核心组件

### LogLevel 枚举
定义日志级别：
- `TRACE`: 最详细的调试信息
- `DEBUG`: 调试信息
- `INFO`: 一般信息（默认级别）
- `WARN`: 警告信息
- `ERROR`: 错误信息
- `CRITICAL`: 严重错误
- `OFF`: 关闭日志

### Logger 类
封装 spdlog，提供模块化的日志接口。

**静态方法：**
- `init(LogLevel default_level, std::string_view pattern)`: 全局初始化，配置所有 logger 的共享 sinks
- `create(std::string_view module_name)`: 创建指定名称的 logger 实例
- `setGlobalLevel(LogLevel level)`: 设置所有 logger 的日志级别
- `flushAll()`: 刷新所有日志输出

**实例方法：**
- `setLevel(LogLevel level)`: 设置当前 logger 的日志级别
- `getLevel() const`: 获取当前 logger 的日志级别
- `addSink(std::shared_ptr<spdlog::sinks::sink> sink)`: 添加输出 sink

**日志输出（支持 fmt 格式化）：**
```cpp
template <typename... Args>
void trace(fmt::format_string<Args...> fmt, Args&&... args)
void debug(fmt::format_string<Args...> fmt, Args&&... args)
void info(fmt::format_string<Args...> fmt, Args&&... args)
void warn(fmt::format_string<Args...> fmt, Args&&... args)
void error(fmt::format_string<Args...> fmt, Args&&... args)
void critical(fmt::format_string<Args...> fmt, Args&&... args)
```

**辅助工具：**
- `logError(Logger& logger, const Exception& err, LogLevel level)`: 将 Exception 输出到日志

## 使用示例

```cpp
// 全局初始化（程序启动时调用一次）
Logger::init(LogLevel::DEBUG);

// 创建模块 logger
Logger renderLog = Logger::create("Renderer");

// 使用日志
renderLog.info("Window created: {}x{}", width, height);
renderLog.warn("Texture not found: {}", texturePath);
renderLog.error("Shader compilation failed: {}", shaderName);

// 日志异常
try {
    // ... 代码
} catch (const Exception& e) {
    logError(renderLog, e, LogLevel::ERROR);
}
```

## 设计要点
1. **模块隔离**: 每个模块创建独立的 logger 实例，便于日志过滤和分析
2. **高性能**: 基于 spdlog 的异步日志机制
3. **格式化**: 集成 fmt 库，提供类型安全的参数化日志
4. **双 sink**: 默认同时输出到控制台和文件
5. **UTF-8 支持**: CMake 中配置 `/utf-8` 选项（MSVC）

## 默认格式
```
[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%-10n] %v
```
- 时间戳（精确到毫秒）
- 日志级别（8 字符左对齐，带颜色）
- 模块名（10 字符左对齐）
- 消息内容

## 测试要点
- 多模块 logger 的隔离性
- 日志级别过滤正确性
- 异常日志输出格式
- 多线程环境下的日志顺序（异步日志）
