# Hash Utilities 模块设计

## 目标
提供哈希函数，用于资源 ID、事件标识等场景。

## 文件结构
```
src/runtime/core/utils/
├── hash.h
└── hash.cpp
```

## 接口设计

```cpp
namespace Incubator::Utils
{
    // FNV-1a 32位哈希（快速，适合资源 ID）
    uint32_t hash32(std::string_view str);
    uint32_t hash32(const void* data, size_t size);

    // FNV-1a 64位哈希
    uint64_t hash64(std::string_view str);
    uint64_t hash64(const void* data, size_t size);

    // 组合哈希（用于复合键）
    template <typename T>
    void hashCombine(size_t& seed, const T& value);

    // 哈希类型别名
    using Hash = uint64_t;

    // 常量哈希（编译时计算）
    constexpr uint32_t constHash32(std::string_view str);
}
```

## 应用场景
- 资源 ID：`Hash id = hash32("textures/player.png")`
- 事件 ID：`Hash eventId = constHash32("PlayerJump")`
- 文件变更检测

## 测试要点
- 哈希分布均匀性（抽样测试）
- 碰撞率检查
- 编译时哈希正确性
