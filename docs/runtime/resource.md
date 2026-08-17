# Resource 资源模块

## 目标

Resource 模块为引擎提供**全局稳定资源标识**与**资源注册/去重**能力。当前阶段聚焦"身份"与"登记"：`ResourceId` 是资源的稳定身份证（会话间不变），`ResourceRegistry` 是全局户口本（去重、哈希碰撞检测、引用计数）。为后续 `Manager`/`ResourceObject`/`Asset` 分层预留接缝。

## 设计原则

1. **身份稳定**：ID 由 `(类型, 规范化虚拟路径)` 计算哈希，不依赖运行时顺序，跨会话可复现
2. **单一真源**：`ResourceRegistry` 是资源注册信息的唯一存储，避免多份拷贝不一致
3. **封闭类型**：`ResourceType` 为封闭枚举，同时充当类型 tag 与未来 Loader 路由键
4. **低层纯数据**：本层不含磁盘 IO 与生命周期状态机（属于上层 Manager/Loader 职责）

## 命名空间

```cpp
namespace Incubator::Resource
```

## 文件结构

```
src/runtime/resource/
├── resource_type.h      # 资源类型枚举（封闭集）
├── resource_id.h/.cpp   # 稳定资源 ID + 虚拟路径规范化
└── resource_registry.h/.cpp  # 全局注册表（去重/碰撞/引用计数）
```

## 核心概念

### 资源类型 `ResourceType`

```cpp
enum class ResourceType : Utils::Hash
{
    TEXTURE = Utils::constHash("TEXTURE"),
    MODEL   = Utils::constHash("MODEL"),
    MATERIAL= Utils::constHash("MATERIAL"),
    SHADER  = Utils::constHash("SHADER"),
    INVALID = 0
};
```

- 底层为 `Utils::Hash`（uint64），显式值 = 字符串 `constHash`，跨会话稳定
- `INVALID = 0` 作为无效类型哨兵
- 枚举值使用 UPPER_CASE，与 `Log::Level` 约定一致

### 稳定资源 ID `ResourceId`

```cpp
class ResourceId
{
public:
    // 由 (类型, 虚拟路径) 计算 ID；非法输入返回无效 ID
    static ResourceId fromPath(ResourceType type, std::string_view virtualPath);

    // 仅由虚拟路径计算 ID（不带类型 tag）
    static ResourceId fromPathOnly(std::string_view virtualPath);

    bool valid() const noexcept;            // 是否为有效 ID
    Utils::Hash value() const noexcept;     // 底层哈希值
    std::string toString() const;           // 十六进制字符串
    static ResourceId fromString(std::string_view hex);  // 反序列化
};
```

- ID 计算：`combine(combine(域种子, 类型tag), hash(规范化键))`
- **域种子** `constHash("Incubator::Resource")` 隔离资源域，避免与其他哈希域冲突
- 值语义，可拷贝；`==`/`<` 运算符支持 map/set key

### 虚拟路径规范化 `canonicalizeResourceKey`

```cpp
std::string canonicalizeResourceKey(std::string_view virtualPath);
```

规范化顺序：`Path::normalize` → 拒绝空/盘符前缀/UNC → 剥离单个前导 `/` → ASCII 小写折叠。非法输入返回空串。

### 全局注册表 `ResourceRegistry`

```cpp
class ResourceRegistry
{
public:
    static ResourceRegistry& instance();   // 全局单例

    void clear();                          // 清空全部条目（测试隔离/引擎关闭）

    // 注册资源（既有则返回原 ID，新建则计算并登记）；非法/碰撞返回无效 ID
    ResourceId registerResource(ResourceType type, std::string_view virtualPath);

    // 低层注入（反序列化恢复/测试碰撞构造）；冲突返回 false
    bool injectEntry(ResourceType type, const std::string& key, ResourceId id);

    void acquire(ResourceId id);           // 引用计数 +1（未注册记录错误日志）
    void release(ResourceId id);           // 引用计数 -1（不会减到负数）
    uint32_t refCount(ResourceId id) const;// 查询引用计数（未注册返回 0）

    // 按 ID 查询 (类型, 虚拟路径)；未注册返回 false
    bool lookup(ResourceId id, ResourceType& type, std::string& virtualPath) const;
    // 按 (类型, 虚拟路径) 查询 ID；未注册返回无效 ID
    ResourceId lookup(ResourceType type, std::string_view virtualPath) const;
};
```

内部结构：
- `m_pathToId`：`(类型, 规范化路径) → id`，支持按路径去重
- `m_idToEntry`：`id → Entry{type, virtualPath, refCount}`，支持按 ID 反查

## 使用场景

| 场景 | 接口 | 说明 |
|------|------|------|
| 资源身份 | `ResourceId::fromPath` | 从路径得到稳定 ID |
| 注册/去重 | `registry.registerResource` | 同一路径只登记一次 |
| 按路径取 ID | `registry.lookup(type, path)` | 反查已注册资源 |
| 按 ID 反查 | `registry.lookup(id, type, path)` | 从持久化 ID 恢复信息 |
| 引用计数 | `acquire` / `release` / `refCount` | 资源生命周期管理 |
| 会话恢复 | `injectEntry` | 重建已持久化的 (type, key, id) 条目 |
| 测试碰撞 | `injectEntry` | 构造哈希碰撞场景 |

## 实现要点

1. `registerResource` 幂等：先查 `m_pathToId`，命中直接返回既有 ID
2. `injectEntry` 双向校验：ID 已被不同 `(type, key)` 占用（碰撞）拒绝；`(type, key)` 已被不同 ID 占用拒绝
3. 引用计数从 1 起步（注册即持有），不会减到负数
4. 错误路径（非法类型/路径/未知 ID）统一通过 `Log::Registry::get("resource")` 记录，不抛异常
5. 线程安全由调用方保证

## 依赖

- `runtime/core/utils/hash.h`：哈希与 `constHash`/`combine`
- `runtime/core/path/path.h`：`Path::normalize` / `Path::toLowerASCII`
- `runtime/core/log/logger.h`：错误日志

## 注意事项

1. `ResourceId` 基于哈希，存在极小碰撞概率；碰撞由注册表检测并拒绝，非安全场景
2. 虚拟路径需为**相对资源根**的路径，盘符/UNC 前缀视为非法
3. 本层不加载实际数据（无磁盘 IO），`contentHash`/`deps`/状态机等属于后续 Asset/ResourceObject 分层
4. `clear()` 会清空全部条目，多测试并行时需注意单例共享