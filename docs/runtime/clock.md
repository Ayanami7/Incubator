# Clock 模块设计

## 目标

Clock 模块是项目的底层基础模块，封装 C++20 `<chrono>` 提供稳定、跨平台的全局时间来源。该模块位于最底层，为后续其他模块（如渲染、物理、动画等）提供统一的时间接口。

## 设计原则

1. **极简性**：仅提供核心时间功能，无任何冗余包装
2. **稳定性**：使用单调时钟，不受系统时间调整影响
3. **跨平台**：基于 C++20 标准，保证各平台行为一致
4. **高性能**：直接调用 steady_clock，零开销抽象

## 命名空间

```cpp
namespace Incubator
```

## 文件结构

```
src/runtime/core/clock/
└── clock.h      // 时钟头文件（内联实现）
```

## 接口设计

```cpp
namespace Incubator
{
    // 时间点类型
    using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

    // 时间间隔类型
    using Duration = std::chrono::steady_clock::duration;

    // 获取当前时间点
    TimePoint now();

    // 计算两个时间点之间的间隔
    Duration elapsedSince(TimePoint start);

    // 获取从某个时间点到现在经过的秒数（浮点）
    double elapsedSeconds(TimePoint start);

    // 获取从某个时间点到现在经过的毫秒数
    int64_t elapsedMilliseconds(TimePoint start);

    // 获取从某个时间点到现在经过的微秒数
    int64_t elapsedMicroseconds(TimePoint start);
}
```

## 应用场景

| 场景 | 使用方式 |
|------|---------|
| 测量函数耗时 | `auto start = Incubator::now(); ... auto elapsed = Incubator::elapsedSeconds(start);` |
| 帧循环计时 | `auto frameStart = Incubator::now();` |
| 延迟检测 | `if (Incubator::elapsedMilliseconds(lastTime) > 1000) { ... }` |

## 实现要点

1. 使用 `std::chrono::steady_clock` 确保单调递增
2. 所有函数内联实现，零额外开销
3. 无状态、无实例、无单例模式

## 依赖

- C++20 `<chrono>` 标准库
- 无其他内部依赖（保持最底层）

## 注意事项

1. Clock 模块不涉及帧率统计、游戏时间管理等高级功能
2. 不提供暂停、时间缩放等游戏特定功能
3. 只提供纯粹的时间获取和计算工具
