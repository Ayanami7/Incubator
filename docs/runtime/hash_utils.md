# Hash Utilities 模块

## 概述

基于 FNV-1a 的 64 位哈希工具模块，用于资源 ID、缓存键、文件变更检测等场景。

```
src/runtime/core/utils/
├── hash.h
└── hash.cpp
```

## 核心接口

```cpp
namespace Incubator::Utils {

    using Hash = uint64_t;

    // 字符串哈希（最常用）
    Hash hash(std::string_view str) noexcept;

    // 快速文件哈希，使用元数据（路径 + size + timestamp）
    Hash hashFileFast(const std::filesystem::path& path) noexcept;

    // 内容文件哈希（完整文件）
    Hash hashFileContent(const std::filesystem::path& path);

    // 编译期哈希
    template <size_t N>
    constexpr Hash constHash(const char (&str)[N]) noexcept;

    // 基础组合（对外提供）
    template <typename T>
    void combine(Hash& seed, const T& value) noexcept;

    // 内部实现（禁止外部使用）
    namespace detail
    {
        Hash hashBytes(const void* data, size_t size) noexcept;
        constexpr Hash kOffset = 14695981039346656037ull;
        constexpr Hash kPrime  = 1099511628211ull;
        constexpr Hash kMul = 0x9e3779b97f4a7c15ull;
    }

}
```

## 使用场景

| 场景 | 推荐接口 | 说明 |
|------|----------|------|
| 资源 ID | `hash()` / `constHash()` | 路径字符串转 ID |
| 缓存键 | `hash()` + `combine()` | 组合多个参数 |
| 文件变更检测 | `hashFileFast()` | 热重载场景，快速检查 |
| 内容校验 | `hashFileContent()` | 完整文件哈希 |
| switch-case | `constHash()` | 编译期常量 |

## 注意事项

- `hashFileFast()` 只检查元数据，可能漏判内容修改
- FNV-1a 哈希存在碰撞概率，勿用于安全场景
- `combine()` 对自定义类型需特化
- `constHash()` 仅支持字面量字符串