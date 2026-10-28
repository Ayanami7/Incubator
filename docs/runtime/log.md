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
| `render` | Renderer 调度、模型 GPU 准备和 ImGui 集成 |
| `render.vulkan` | Vulkan Instance、Device、Pipeline、上传和帧同步 |

`Registry::get()` 按完整名称区分大小写；点分隔仅用于分类，不自动继承父模块的级别或 sink。
新增内置模块沿用上述命名，不混用 `render`、`Render` 或 `Renderer`。

## 调用约定

按有诊断价值的事件记录日志，不按函数数、代码行数或固定数量要求覆盖。优先复用现有模块名和日志接口，不为补充日志增加通用追踪、限流或统计框架。

| 级别 | 使用约定 |
|---|---|
| Info | 用户可感知的操作结果及重要会话状态变化，例如场景保存/加载、设备选择和交换链重建 |
| Debug | 加载、解码、模型准备、上传及目标资源创建/重建的摘要，避免同一成功路径逐层打印相同内容 |
| Warn | 应用继续运行，但发生需要关注的降级或异常状态，例如可选 Validation 不可用 |
| Error | 操作失败或必须处理的底层错误，包含失败对象、原因与可用上下文 |
| Trace | 只用于有明确排查目的的细节追踪；高频调试输出应由实际需求驱动，不默认开启 |

### 错误记录边界

- 由决定如何处理失败的边界负责记录。仅补充异常上下文并继续抛出的底层代码不重复打印；清理后继续抛出的代码也不额外记录同一原因。
- ResourceManager 将 Loader 异常转为 nullptr 时记录一次；Loader 直接抛出的读取/解析异常交给调用方处理。现有已诊断失败返回 nullptr 的约定继续有效，Manager 不重复打印。
- 应用终止、用户打开/保存失败等边界负责记录操作结果。若低层已记录具体资源失败，高层摘要应补充不同的操作上下文，避免再次原样展开同一错误。
- 涉及资源、文件或 GPU 时，使用已有的路径、资源类型/ID、尺寸、格式或底层错误码等相关信息；不为所有日志强制收集无关字段。可取得上下文时，不只输出“失败”或“未知错误”。
- 合法的空场景、隐藏/离屏实体、零尺寸跳帧和普通缓存命中不属于警告或错误。

### 频率与摘要

- 正常逐帧、逐实体、逐输入事件不输出 Info；普通缓存命中不逐次打印日志。
- 资源创建与重建摘要只在实际执行并成功后记录，复用已有结果时不打印。当前模型准备完成摘要使用 `render` Debug，包含规范化路径、实际 GPU 资源数量及请求的准备选项；源材质可能保留 Unlit，不能把请求选项当作每个实际材质的最终模式。
- 视口目标摘要使用 `render` Debug，包含帧槽、旧/新尺寸和颜色/深度格式；稳定帧不输出，连续调整尺寸可按实际重建记录。
- 持续状态优先在状态变化时记录，避免在正常循环中重复打印同一状态。保留 Vulkan Validation 等外部诊断回调的原始信息，不因应用侧的频率规则丢弃其报告。

## 使用示例

```cpp
// 全局初始化（程序启动时调用一次）
Log::Registry::init(Log::Level::DEBUG);

// 获取模块 logger
auto renderLog = Log::Registry::get("render");

// 使用日志
renderLog.info("Window created: {}x{}", width, height);
renderLog.warn("Optional validation unavailable; continuing without validation");
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
