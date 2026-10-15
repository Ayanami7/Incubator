# Logger 模块设计

## 目标
基于 spdlog 提供高性能、模块化的日志系统，支持控制台/文件多 sink 与轮转文件日志。

## 文件结构
```
src/runtime/core/log/
├── logger.h
└── logger.cpp
```

## 命名空间

```cpp
namespace Incubator::Log
```

## 核心组件

### Level 枚举

```cpp
enum class Level : int
{
    TRACE = 0,     // 最详细的调试信息（仅开发期使用）
    DEBUG = 1,     // 调试信息
    INFO = 2,      // 一般信息（默认级别）
    WARN = 3,      // 警告信息
    ERROR = 4,     // 错误信息
    CRITICAL = 5,  // 严重错误
    OFF = 6        // 关闭所有日志
};
```

显式值与 `spdlog::level::level_enum` 对齐，便于序列化和映射。级别使用 UPPER_CASE，与 `ResourceType` 约定一致。

### FileSinkConfig 文件日志配置

```cpp
struct FileSinkConfig
{
    std::string log_dir;          // 日志输出目录，为空则禁用文件日志
    std::string basename = "app"; // 日志文件名（不含路径和扩展名）
    size_t max_size = 5 * 1024 * 1024;  // 单文件最大字节数，0=不轮转
    size_t max_files = 3;               // 轮转保留的历史文件数
};
```

### Registry 类（全局单例）

`log_dir` 与 `basename` 均采用 UTF-8。构造文件路径时统一经 `Utils::Utf8` 转换；Windows
启用 spdlog 的 `SPDLOG_WCHAR_FILENAMES`，以原生 UTF-16 路径打开和轮转文件，不经过系统 ANSI 代码页。

`max_size == 0` 使用普通文件 sink 持续追加，不轮转；此时 `max_files` 不参与限制。
`max_size > 0` 使用轮转文件 sink，并按 `max_files` 保留历史文件。

管理所有 logger 的共享状态（sinks、级别、缓存）。所有静态方法线程安全。

**静态方法：**
```cpp
static void init(Level default_level = Level::INFO,
                 std::string_view pattern = {},
                 const FileSinkConfig& file_config = {});
// 全局初始化：配置控制台 sink（可自定义 pattern）与可选文件 sink

static Logger get(std::string_view module_name);
// 获取指定模块的 Logger（相同名称返回同一实例）；未 init 时自动懒初始化

static void setGlobalLevel(Level level);      // 设置所有 logger 的日志级别
static void setFileSinkConfig(const FileSinkConfig& config);
// 运行时添加/移除文件 sink（log_dir 为空则移除文件输出），可在 init 之后调用

static void flushAll();   // 刷新所有日志输出到 sink
static void reset();      // 重置日志系统（清理所有 logger 和 sink）
```

### Logger 类（轻量值类型句柄）

各模块持有的日志输出接口，内部持 `shared_ptr<spdlog::logger>`，拷贝开销低。

**实例方法：**
```cpp
bool isValid() const;                       // 是否持有有效的 spdlog::logger
void setLevel(Level level);                 // 设置此 logger 的日志级别
Level getLevel() const;                     // 获取此 logger 的当前级别
void addSink(std::shared_ptr<spdlog::sinks::sink> sink);  // 添加自定义 sink
```

**日志输出（支持 fmt 格式化）：**
```cpp
template <typename... Args>
void trace(fmt::format_string<Args...> fmt, Args&&... args)
void debug(...) / info(...) / warn(...) / error(...) / critical(...)
```

**辅助函数：**
```cpp
void logError(Logger& logger, const Error::Exception& err, Level level);
// 将 Exception 输出到日志
```

## 模块命名

内置日志模块使用全小写名称，子模块用点分隔，与职责对应：

| 模块名 | 使用范围 |
|---|---|
| `demo` | Demo 启动、场景选择与关闭 |
| `resource` | 资源登记、加载、解码与卸载 |
| `platform.window` | 窗口创建参数与 GLFW 错误 |
| `render` | Renderer 调度和 ImGui 集成 |
| `render.vulkan` | Vulkan Instance、Device、Pipeline、上传和帧同步 |

`Registry::get()` 按完整名称区分大小写；点分隔仅用于分类，不自动继承父模块的级别或 sink。
新增内置模块沿用上述命名，不混用 `render`、`Render` 或 `Renderer`。

## 使用示例

```cpp
// 全局初始化（程序启动时调用一次）
Log::Registry::init(Log::Level::DEBUG);

// 获取模块 logger
auto renderLog = Log::Registry::get("render");

// 使用日志
renderLog.info("Window created: {}x{}", width, height);
renderLog.warn("Texture not found: {}", texturePath);
renderLog.error("Shader compilation failed: {}", shaderName);

// 日志异常
try
{
    // ... 代码
}
catch (const Error::Exception& e)
{
    Log::logError(renderLog, e, Log::Level::ERROR);
}
```

## 设计要点

1. **职责分离**：`Registry` 管理全局共享状态，`Logger` 作为轻量句柄供各模块使用
2. **模块隔离**：每个模块通过名称获取独立 logger，便于日志过滤和分析
3. **懒初始化**：`get()` 在未调用 `init()` 时自动使用默认配置初始化，保证随时可用
4. **格式化**：集成 fmt 库，提供类型安全的参数化日志
5. **线程安全**：mutex 保护注册表；带锁的分发 sink 管理全局输出与各模块自定义输出，可在日志输出/刷新期间安全添加自定义 sink、启用或移除文件输出。spdlog logger 的 sink 列表在创建后不再修改；自定义 sink 自身也应支持多线程使用
6. **文件日志可热插拔**：`setFileSinkConfig` 支持运行时添加/移除文件输出

## 默认格式

```
[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%n] %v
```

- 时间戳（精确到毫秒）
- 日志级别（8 字符左对齐，带颜色）
- 模块名
- 消息内容

## 测试要点
- 多模块 logger 的隔离性
- 日志级别过滤正确性
- 异常日志输出格式
- 多线程环境下的日志顺序
